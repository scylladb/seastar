/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */


/** @file */

#pragma once

#include <seastar/core/task.hh>
#include <seastar/core/future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/timer.hh>
#include <seastar/util/bool_class.hh>
#include <tuple>
#include <iterator>
#include <vector>
#include <seastar/util/std-compat.hh>
#include <seastar/util/tuple_utils.hh>
#include <seastar/util/noncopyable_function.hh>

namespace seastar {

/// \cond internal
extern __thread size_t task_quota;
/// \endcond


/// \cond internal
namespace internal {

template <typename Func>
void
schedule_in_group(scheduling_group sg, Func func) {
    schedule(make_task(sg, std::move(func)));
}


}
/// \endcond

/// \addtogroup future-util
/// @{

/// \brief run a callable (with some arbitrary arguments) in a scheduling group
///
/// If the conditions are suitable (see scheduling_group::may_run_immediately()),
/// then the function is run immediately. Otherwise, the function is queued to run
/// when its scheduling group next runs.
///
/// \param sg  scheduling group that controls execution time for the function
/// \param func function to run; must be movable or copyable
/// \param args arguments to the function; may be copied or moved, so use \c std::ref()
///             to force passing references
template <typename Func, typename... Args>
inline
auto
with_scheduling_group(scheduling_group sg, Func func, Args&&... args) {
    using return_type = decltype(func(std::forward<Args>(args)...));
    using futurator = futurize<return_type>;
    if (sg.active()) {
        return futurator::invoke(func, std::forward<Args>(args)...);
    } else {
        typename futurator::promise_type pr;
        auto f = pr.get_future();
        internal::schedule_in_group(sg, [pr = std::move(pr), func = std::move(func), args = std::make_tuple(std::forward<Args>(args)...)] () mutable {
            return futurator::apply(func, std::move(args)).forward_to(std::move(pr));
        });
        return f;
    }
}

namespace internal {

template <typename Iterator, typename IteratorCategory>
inline
size_t
iterator_range_estimate_vector_capacity(Iterator begin, Iterator end, IteratorCategory category) {
    // For InputIterators we can't estimate needed capacity
    return 0;
}

template <typename Iterator>
inline
size_t
iterator_range_estimate_vector_capacity(Iterator begin, Iterator end, std::forward_iterator_tag category) {
    // May be linear time below random_access_iterator_tag, but still better than reallocation
    return std::distance(begin, end);
}

}

/// \cond internal

class parallel_for_each_state final : private continuation_base<> {
    std::vector<future<>> _incomplete;
    promise<> _result;
    std::exception_ptr _ex;
private:
    // Wait for one of the futures in _incomplete to complete, and then
    // decide what to do: wait for another one, or deliver _result if all
    // are complete.
    void wait_for_one() noexcept;
    virtual void run_and_dispose() noexcept override;
public:
    parallel_for_each_state(size_t n);
    void add_future(future<>&& f);
    future<> get_future();
};

/// \endcond

/// Run tasks in parallel (iterator version).
///
/// Given a range [\c begin, \c end) of objects, run \c func on each \c *i in
/// the range, and return a future<> that resolves when all the functions
/// complete.  \c func should return a future<> that indicates when it is
/// complete.  All invocations are performed in parallel. This allows the range
/// to refer to stack objects, but means that unlike other loops this cannot
/// check need_preempt and can only be used with small ranges.
///
/// \param begin an \c InputIterator designating the beginning of the range
/// \param end an \c InputIterator designating the end of the range
/// \param func Function to invoke with each element in the range (returning
///             a \c future<>)
/// \return a \c future<> that resolves when all the function invocations
///         complete.  If one or more return an exception, the return value
///         contains one of the exceptions.
template <typename Iterator, typename Func>
SEASTAR_CONCEPT( requires requires (Func f, Iterator i) { { f(*i++) } -> std::same_as<future<>>; } )
inline
future<>
parallel_for_each(Iterator begin, Iterator end, Func&& func) noexcept {
    parallel_for_each_state* s = nullptr;
    // Process all elements, giving each future the following treatment:
    //   - available, not failed: do nothing
    //   - available, failed: collect exception in ex
    //   - not available: collect in s (allocating it if needed)
    while (begin != end) {
        auto f = futurize_invoke(std::forward<Func>(func), *begin++);
        if (!f.available() || f.failed()) {
            if (!s) {
                using itraits = std::iterator_traits<Iterator>;
                auto n = (internal::iterator_range_estimate_vector_capacity(begin, end, typename itraits::iterator_category()) + 1);
                s = new parallel_for_each_state(n);
            }
            s->add_future(std::move(f));
        }
    }
    // If any futures were not available, hand off to parallel_for_each_state::start().
    // Otherwise we can return a result immediately.
    if (s) {
        // s->get_future() takes ownership of s (and chains it to one of the futures it contains)
        // so this isn't a leak
        return s->get_future();
    }
    return make_ready_future<>();
}

/// Run tasks in parallel (range version).
///
/// Given a \c range of objects, invoke \c func with each object
/// in the range, and return a future<> that resolves when all
/// the functions complete.  \c func should return a future<> that indicates
/// when it is complete.  All invocations are performed in parallel. This allows
/// the range to refer to stack objects, but means that unlike other loops this
/// cannot check need_preempt and can only be used with small ranges.
///
/// \param range A range of objects to iterate run \c func on
/// \param func  A callable, accepting reference to the range's
///              \c value_type, and returning a \c future<>.
/// \return a \c future<> that becomes ready when the entire range
///         was processed.  If one or more of the invocations of
///         \c func returned an exceptional future, then the return
///         value will contain one of those exceptions.

/// \cond internal
namespace internal {

template <typename Range, typename Func>
inline
future<>
parallel_for_each_impl(Range&& range, Func&& func) {
    return parallel_for_each(std::begin(range), std::end(range),
            std::forward<Func>(func));
}

} // namespace internal
/// \endcond

template <typename Range, typename Func>
SEASTAR_CONCEPT( requires requires (Func f, Range r) { { f(*r.begin()) } -> std::same_as<future<>>; } )
inline
future<>
parallel_for_each(Range&& range, Func&& func) noexcept {
    auto impl = internal::parallel_for_each_impl<Range, Func>;
    return futurize_invoke(impl, std::forward<Range>(range), std::forward<Func>(func));
}

// The AsyncAction concept represents an action which can complete later than
// the actual function invocation. It is represented by a function which
// returns a future which resolves when the action is done.

struct stop_iteration_tag { };
using stop_iteration = bool_class<stop_iteration_tag>;

/// \cond internal

namespace internal {

template <typename AsyncAction>
class repeater final : public continuation_base<stop_iteration> {
    promise<> _promise;
    AsyncAction _action;
public:
    explicit repeater(AsyncAction action) : _action(std::move(action)) {}
    repeater(stop_iteration si, AsyncAction action) : repeater(std::move(action)) {
        _state.set(std::make_tuple(si));
    }
    future<> get_future() { return _promise.get_future(); }
    virtual void run_and_dispose() noexcept override {
        if (_state.failed()) {
            _promise.set_exception(std::move(_state).get_exception());
            delete this;
            return;
        } else {
            if (std::get<0>(_state.get()) == stop_iteration::yes) {
                _promise.set_value();
                delete this;
                return;
            }
            _state = {};
        }
        try {
            do {
                auto f = futurize_invoke(_action);
                if (!f.available()) {
                    internal::set_callback(f, this);
                    return;
                }
                if (f.get0() == stop_iteration::yes) {
                    _promise.set_value();
                    delete this;
                    return;
                }
            } while (!need_preempt());
        } catch (...) {
            _promise.set_exception(std::current_exception());
            delete this;
            return;
        }
        _state.set(stop_iteration::no);
        schedule(this);
    }
};

}

/// \endcond

/// Invokes given action until it fails or the function requests iteration to stop by returning
/// \c stop_iteration::yes.
///
/// \param action a callable taking no arguments, returning a future<stop_iteration>.  Will
///               be called again as soon as the future resolves, unless the
///               future fails, action throws, or it resolves with \c stop_iteration::yes.
///               If \c action is an r-value it can be moved in the middle of iteration.
/// \return a ready future if we stopped successfully, or a failed future if
///         a call to to \c action failed.
template<typename AsyncAction>
SEASTAR_CONCEPT( requires seastar::InvokeReturns<AsyncAction, stop_iteration> || seastar::InvokeReturns<AsyncAction, future<stop_iteration>> )
inline
future<> repeat(AsyncAction action) noexcept {
    using futurator = futurize<std::result_of_t<AsyncAction()>>;
    static_assert(std::is_same<future<stop_iteration>, typename futurator::type>::value, "bad AsyncAction signature");
    try {
        do {
            // Do not type-erase here in case this is a short repeat()
            auto f = futurator::invoke(action);

            if (!f.available()) {
              return [&] () noexcept {
                memory::disable_failure_guard dfg;
                auto repeater = new internal::repeater<AsyncAction>(std::move(action));
                auto ret = repeater->get_future();
                internal::set_callback(f, repeater);
                return ret;
              }();
            }

            if (f.get0() == stop_iteration::yes) {
                return make_ready_future<>();
            }
        } while (!need_preempt());

        auto repeater = new internal::repeater<AsyncAction>(stop_iteration::no, std::move(action));
        auto ret = repeater->get_future();
        schedule(repeater);
        return ret;
    } catch (...) {
        return make_exception_future(std::current_exception());
    }
}

/// \cond internal

template <typename T>
struct repeat_until_value_type_helper;

/// \endcond

/// Type helper for repeat_until_value()
template <typename T>
struct repeat_until_value_type_helper<future<compat::optional<T>>> {
    /// The type of the value we are computing
    using value_type = T;
    /// Type used by \c AsyncAction while looping
    using optional_type = compat::optional<T>;
    /// Return type of repeat_until_value()
    using future_type = future<value_type>;
};

/// Return value of repeat_until_value()
template <typename AsyncAction>
using repeat_until_value_return_type
        = typename repeat_until_value_type_helper<typename futurize<std::result_of_t<AsyncAction()>>::type>::future_type;

namespace internal {

template <typename AsyncAction, typename T>
class repeat_until_value_state final : public continuation_base<compat::optional<T>> {
    promise<T> _promise;
    AsyncAction _action;
public:
    explicit repeat_until_value_state(AsyncAction action) : _action(std::move(action)) {}
    repeat_until_value_state(compat::optional<T> st, AsyncAction action) : repeat_until_value_state(std::move(action)) {
        this->_state.set(std::make_tuple(std::move(st)));
    }
    future<T> get_future() { return _promise.get_future(); }
    virtual void run_and_dispose() noexcept override {
        if (this->_state.failed()) {
            _promise.set_exception(std::move(this->_state).get_exception());
            delete this;
            return;
        } else {
            auto v = std::get<0>(std::move(this->_state).get());
            if (v) {
                _promise.set_value(std::move(*v));
                delete this;
                return;
            }
            this->_state = {};
        }
        try {
            do {
                auto f = futurize_invoke(_action);
                if (!f.available()) {
                    internal::set_callback(f, this);
                    return;
                }
                auto ret = f.get0();
                if (ret) {
                    _promise.set_value(std::make_tuple(std::move(*ret)));
                    delete this;
                    return;
                }
            } while (!need_preempt());
        } catch (...) {
            _promise.set_exception(std::current_exception());
            delete this;
            return;
        }
        this->_state.set(compat::nullopt);
        schedule(this);
    }
};

}

/// Invokes given action until it fails or the function requests iteration to stop by returning
/// an engaged \c future<compat::optional<T>> or compat::optional<T>.  The value is extracted
/// from the \c optional, and returned, as a future, from repeat_until_value().
///
/// \param action a callable taking no arguments, returning a future<compat::optional<T>>
///               or compat::optional<T>.  Will be called again as soon as the future
///               resolves, unless the future fails, action throws, or it resolves with
///               an engaged \c optional.  If \c action is an r-value it can be moved
///               in the middle of iteration.
/// \return a ready future if we stopped successfully, or a failed future if
///         a call to to \c action failed.  The \c optional's value is returned.
template<typename AsyncAction>
SEASTAR_CONCEPT( requires requires (AsyncAction aa) {
    bool(futurize_invoke(aa).get0());
    futurize_invoke(aa).get0().value();
} )
repeat_until_value_return_type<AsyncAction>
repeat_until_value(AsyncAction action) noexcept {
    using futurator = futurize<std::result_of_t<AsyncAction()>>;
    using type_helper = repeat_until_value_type_helper<typename futurator::type>;
    // the "T" in the documentation
    using value_type = typename type_helper::value_type;
    using optional_type = typename type_helper::optional_type;
    do {
        auto f = futurator::invoke(action);

        if (!f.available()) {
          return [&] () noexcept {
            memory::disable_failure_guard dfg;
            auto state = new internal::repeat_until_value_state<AsyncAction, value_type>(std::move(action));
            auto ret = state->get_future();
            internal::set_callback(f, state);
            return ret;
          }();
        }

        if (f.failed()) {
            return make_exception_future<value_type>(f.get_exception());
        }

        optional_type&& optional = std::move(f).get0();
        if (optional) {
            return make_ready_future<value_type>(std::move(optional.value()));
        }
    } while (!need_preempt());

    try {
        auto state = new internal::repeat_until_value_state<AsyncAction, value_type>(compat::nullopt, std::move(action));
        auto f = state->get_future();
        schedule(state);
        return f;
    } catch (...) {
        return make_exception_future<value_type>(std::current_exception());
    }
}

namespace internal {

template <typename StopCondition, typename AsyncAction>
class do_until_state final : public continuation_base<> {
    promise<> _promise;
    StopCondition _stop;
    AsyncAction _action;
public:
    explicit do_until_state(StopCondition stop, AsyncAction action) : _stop(std::move(stop)), _action(std::move(action)) {}
    future<> get_future() { return _promise.get_future(); }
    virtual void run_and_dispose() noexcept override {
        if (_state.available()) {
            if (_state.failed()) {
                _promise.set_urgent_state(std::move(_state));
                delete this;
                return;
            }
            _state = {}; // allow next cycle to overrun state
        }
        try {
            do {
                if (_stop()) {
                    _promise.set_value();
                    delete this;
                    return;
                }
                auto f = _action();
                if (!f.available()) {
                    internal::set_callback(f, this);
                    return;
                }
                if (f.failed()) {
                    f.forward_to(std::move(_promise));
                    delete this;
                    return;
                }
            } while (!need_preempt());
        } catch (...) {
            _promise.set_exception(std::current_exception());
            delete this;
            return;
        }
        schedule(this);
    }
};

}
    
/// Invokes given action until it fails or given condition evaluates to true.
///
/// \param stop_cond a callable taking no arguments, returning a boolean that
///                  evalutes to true when you don't want to call \c action
///                  any longer
/// \param action a callable taking no arguments, returning a future<>.  Will
///               be called again as soon as the future resolves, unless the
///               future fails, or \c stop_cond returns \c true.
/// \return a ready future if we stopped successfully, or a failed future if
///         a call to to \c action failed.
template<typename AsyncAction, typename StopCondition>
SEASTAR_CONCEPT( requires seastar::InvokeReturns<StopCondition, bool> && seastar::InvokeReturns<AsyncAction, future<>> )
inline
future<> do_until(StopCondition stop_cond, AsyncAction action) noexcept {
    using namespace internal;
    do {
        if (stop_cond()) {
            return make_ready_future<>();
        }
        auto f = futurize_invoke(action);
        if (!f.available()) {
          return [&] () noexcept {
            memory::disable_failure_guard dfg;
            auto task = new do_until_state<StopCondition, AsyncAction>(std::move(stop_cond), std::move(action));
            auto ret = task->get_future();
            internal::set_callback(f, task);
            return ret;
          }();
        }
        if (f.failed()) {
            return f;
        }
    } while (!need_preempt());

    auto task = new do_until_state<StopCondition, AsyncAction>(std::move(stop_cond), std::move(action));
    auto f = task->get_future();
    schedule(task);
    return f;
}

/// Invoke given action until it fails.
///
/// Calls \c action repeatedly until it returns a failed future.
///
/// \param action a callable taking no arguments, returning a \c future<>
///        that becomes ready when you wish it to be called again.
/// \return a future<> that will resolve to the first failure of \c action
template<typename AsyncAction>
SEASTAR_CONCEPT( requires seastar::InvokeReturns<AsyncAction, future<>> )
inline
future<> keep_doing(AsyncAction action) noexcept {
    return repeat([action = std::move(action)] () mutable {
        return action().then([] {
            return stop_iteration::no;
        });
    });
}

namespace internal {
template <typename Iterator, typename AsyncAction>
class do_for_each_state final : public continuation_base<> {
    Iterator _begin;
    Iterator _end;
    AsyncAction _action;
    promise<> _pr;

public:
    do_for_each_state(Iterator begin, Iterator end, AsyncAction action, future<> first_unavailable)
        : _begin(std::move(begin)), _end(std::move(end)), _action(std::move(action)) {
        internal::set_callback(first_unavailable, this);
    }
    virtual void run_and_dispose() noexcept override {
        std::unique_ptr<do_for_each_state> zis(this);
        if (_state.failed()) {
            _pr.set_urgent_state(std::move(_state));
            return;
        }
        while (_begin != _end) {
            auto f = futurize_invoke(_action, *_begin++);
            if (f.failed()) {
                f.forward_to(std::move(_pr));
                return;
            }
            if (!f.available() || need_preempt()) {
                _state = {};
                internal::set_callback(f, this);
                zis.release();
                return;
            }
        }
        _pr.set_value();
    }
    future<> get_future() {
        return _pr.get_future();
    }
};

template<typename Iterator, typename AsyncAction>
inline
future<> do_for_each_impl(Iterator begin, Iterator end, AsyncAction action) {
    while (begin != end) {
        auto f = futurize_invoke(action, *begin++);
        if (f.failed()) {
            return f;
        }
        if (!f.available() || need_preempt()) {
            auto* s = new internal::do_for_each_state<Iterator, AsyncAction>{
                std::move(begin), std::move(end), std::move(action), std::move(f)};
            return s->get_future();
        }
    }
    return make_ready_future<>();
}
} // namespace internal

/// Call a function for each item in a range, sequentially (iterator version).
///
/// For each item in a range, call a function, waiting for the previous
/// invocation to complete before calling the next one.
///
/// \param begin an \c InputIterator designating the beginning of the range
/// \param end an \c InputIterator designating the endof the range
/// \param action a callable, taking a reference to objects from the range
///               as a parameter, and returning a \c future<> that resolves
///               when it is acceptable to process the next item.
/// \return a ready future on success, or the first failed future if
///         \c action failed.
template<typename Iterator, typename AsyncAction>
SEASTAR_CONCEPT( requires requires (Iterator i, AsyncAction aa) {
    { futurize_invoke(aa, *i) } -> std::same_as<future<>>;
} )
inline
future<> do_for_each(Iterator begin, Iterator end, AsyncAction action) noexcept {
    try {
        return internal::do_for_each_impl(std::move(begin), std::move(end), std::move(action));
    } catch (...) {
        return current_exception_as_future();
    }
}

/// Call a function for each item in a range, sequentially (range version).
///
/// For each item in a range, call a function, waiting for the previous
/// invocation to complete before calling the next one.
///
/// \param range an \c Range object designating input values
/// \param action a callable, taking a reference to objects from the range
///               as a parameter, and returning a \c future<> that resolves
///               when it is acceptable to process the next item.
/// \return a ready future on success, or the first failed future if
///         \c action failed.
template<typename Container, typename AsyncAction>
SEASTAR_CONCEPT( requires requires (Container c, AsyncAction aa) {
    { futurize_invoke(aa, *c.begin()) } -> std::same_as<future<>>;
} )
inline
future<> do_for_each(Container& c, AsyncAction action) noexcept {
    try {
        return internal::do_for_each_impl(std::begin(c), std::end(c), std::move(action));
    } catch (...) {
        return current_exception_as_future();
    }
}

/// \cond internal
namespace internal {

template<typename... Futures>
struct identity_futures_tuple {
    using future_type = future<std::tuple<Futures...>>;
    using promise_type = typename future_type::promise_type;

    static void set_promise(promise_type& p, std::tuple<Futures...> futures) {
        p.set_value(std::move(futures));
    }

    static future_type make_ready_future(std::tuple<Futures...> futures) noexcept {
        return futurize<future_type>::from_tuple(std::move(futures));
    }

    static future_type current_exception_as_future() noexcept {
        return seastar::current_exception_as_future<std::tuple<Futures...>>();
    }
};

// Given a future type, find the continuation_base corresponding to that future
template <typename Future>
struct continuation_base_for_future;

template <typename... T>
struct continuation_base_for_future<future<T...>> {
    using type = continuation_base<T...>;
};

template <typename Future>
using continuation_base_for_future_t = typename continuation_base_for_future<Future>::type;

class when_all_state_base;

// If the future is ready, return true
// if the future is not ready, chain a continuation to it, and return false
using when_all_process_element_func = bool (*)(void* future, void* continuation, when_all_state_base* wasb);

struct when_all_process_element {
    when_all_process_element_func func;
    void* future;
};

class when_all_state_base {
    size_t _nr_remain;
    const when_all_process_element* _processors;
    void* _continuation;
public:
    virtual ~when_all_state_base() {}
    when_all_state_base(size_t nr_remain, const when_all_process_element* processors, void* continuation)
            : _nr_remain(nr_remain), _processors(processors), _continuation(continuation) {
    }
    void complete_one() {
        // We complete in reverse order; if the futures happen to complete
        // in order, then waiting for the last one will find the rest ready
        --_nr_remain;
        while (_nr_remain) {
            bool ready = process_one(_nr_remain - 1);
            if (!ready) {
                return;
            }
            --_nr_remain;
        }
        if (!_nr_remain) {
            delete this;
        }
    }
    void do_wait_all() {
        ++_nr_remain; // fake pending completion for complete_one()
        complete_one();
    }
    bool process_one(size_t idx) {
        auto p = _processors[idx];
        return p.func(p.future, _continuation, this);
    }
};

template <typename Future>
class when_all_state_component final : public continuation_base_for_future_t<Future> {
    when_all_state_base* _base;
    Future* _final_resting_place;
public:
    static bool process_element_func(void* future, void* continuation, when_all_state_base* wasb) noexcept {
        auto f = reinterpret_cast<Future*>(future);
        if (f->available()) {
            return true;
        } else {
            auto c = new (continuation) when_all_state_component(wasb, f);
            set_callback(*f, c);
            return false;
        }
    }
    when_all_state_component(when_all_state_base *base, Future* future) : _base(base), _final_resting_place(future) {}
    virtual void run_and_dispose() noexcept override {
        using futurator = futurize<Future>;
        if (__builtin_expect(this->_state.failed(), false)) {
            *_final_resting_place = futurator::make_exception_future(std::move(this->_state).get_exception());
        } else {
            *_final_resting_place = futurator::from_tuple(std::move(this->_state).get_value());
        }
        auto base = _base;
        this->~when_all_state_component();
        base->complete_one();
    }
};

#if __cpp_fold_expressions >= 201603
// This optimization requires C++17
#  define SEASTAR__WAIT_ALL__AVOID_ALLOCATION_WHEN_ALL_READY
#endif

template<typename ResolvedTupleTransform, typename... Futures>
class when_all_state : public when_all_state_base {
    static constexpr size_t nr = sizeof...(Futures);
    using type = std::tuple<Futures...>;
    type tuple;
    // We only schedule one continuation at a time, and store it in _cont.
    // This way, if while the future we wait for completes, some other futures
    // also complete, we won't need to schedule continuations for them.
    std::aligned_union_t<1, when_all_state_component<Futures>...> _cont;
    when_all_process_element _processors[nr];
public:
    typename ResolvedTupleTransform::promise_type p;
    when_all_state(Futures&&... t) : when_all_state_base(nr, _processors, &_cont), tuple(std::make_tuple(std::move(t)...)) {
        init_element_processors(std::make_index_sequence<nr>());
    }
    virtual ~when_all_state() {
        ResolvedTupleTransform::set_promise(p, std::move(tuple));
    }
private:
    template <size_t... Idx>
    void init_element_processors(std::index_sequence<Idx...>) {
        auto ignore = {
	    0,
            (_processors[Idx] = when_all_process_element{
                when_all_state_component<std::tuple_element_t<Idx, type>>::process_element_func,
                &std::get<Idx>(tuple)
	     }, 0)...
        };
        (void)ignore;
    }
public:
    static typename ResolvedTupleTransform::future_type wait_all(Futures&&... futures) noexcept {
#ifdef SEASTAR__WAIT_ALL__AVOID_ALLOCATION_WHEN_ALL_READY
        if ((futures.available() && ...)) {
            return ResolvedTupleTransform::make_ready_future(std::make_tuple(std::move(futures)...));
        }
#endif
        auto state = [&] () noexcept {
            memory::disable_failure_guard dfg;
            return new when_all_state(std::move(futures)...);
        }();
        auto ret = state->p.get_future();
        state->do_wait_all();
        return ret;
    }
};

}
/// \endcond

SEASTAR_CONCEPT(

/// \cond internal
namespace impl {


// Want: folds

template <typename T>
struct is_tuple_of_futures : std::false_type {
};

template <>
struct is_tuple_of_futures<std::tuple<>> : std::true_type {
};

template <typename... T, typename... Rest>
struct is_tuple_of_futures<std::tuple<future<T...>, Rest...>> : is_tuple_of_futures<std::tuple<Rest...>> {
};

}
/// \endcond

template <typename... Futs>
concept AllAreFutures = impl::is_tuple_of_futures<std::tuple<Futs...>>::value;

)

template<typename Fut, std::enable_if_t<is_future<Fut>::value, int> = 0>
auto futurize_invoke_if_func(Fut&& fut) noexcept {
    return std::forward<Fut>(fut);
}

template<typename Func, std::enable_if_t<!is_future<Func>::value, int> = 0>
auto futurize_invoke_if_func(Func&& func) noexcept {
    return futurize_invoke(std::forward<Func>(func));
}

namespace internal {

template <typename... Futs>
SEASTAR_CONCEPT( requires seastar::AllAreFutures<Futs...> )
inline
future<std::tuple<Futs...>>
when_all_impl(Futs&&... futs) noexcept {
    using state = when_all_state<identity_futures_tuple<Futs...>, Futs...>;
    return state::wait_all(std::forward<Futs>(futs)...);
}

} // namespace internal

/// Wait for many futures to complete, capturing possible errors (variadic version).
///
/// Each future can be passed directly, or a function that returns a
/// future can be given instead.
///
/// If any function throws, an exceptional future is created for it.
///
/// Returns a tuple of futures so individual values or exceptions can be
/// examined.
///
/// \param fut_or_funcs futures or functions that return futures
/// \return an \c std::tuple<> of all futures returned; when ready,
///         all contained futures will be ready as well.
template <typename... FutOrFuncs>
inline auto when_all(FutOrFuncs&&... fut_or_funcs) noexcept {
    return internal::when_all_impl(futurize_invoke_if_func(std::forward<FutOrFuncs>(fut_or_funcs))...);
}

/// \cond internal
namespace internal {

template<typename Future>
struct identity_futures_vector {
    using future_type = future<std::vector<Future>>;
    static future_type run(std::vector<Future> futures) noexcept {
        return make_ready_future<std::vector<Future>>(std::move(futures));
    }
    static future_type current_exception_as_future() noexcept {
        return seastar::current_exception_as_future<std::vector<Future>>();
    }
};

// Internal function for when_all().
template <typename ResolvedVectorTransform, typename Future>
inline
typename ResolvedVectorTransform::future_type
complete_when_all(std::vector<Future>&& futures, typename std::vector<Future>::iterator pos) noexcept {
    // If any futures are already ready, skip them.
    while (pos != futures.end() && pos->available()) {
        ++pos;
    }
    // Done?
    if (pos == futures.end()) {
        return ResolvedVectorTransform::run(std::move(futures));
    }
    // Wait for unready future, store, and continue.
    return pos->then_wrapped([futures = std::move(futures), pos] (auto fut) mutable {
        *pos++ = std::move(fut);
        return complete_when_all<ResolvedVectorTransform>(std::move(futures), pos);
    });
}

template<typename ResolvedVectorTransform, typename FutureIterator>
inline auto
do_when_all(FutureIterator begin, FutureIterator end) noexcept {
    using itraits = std::iterator_traits<FutureIterator>;
    auto make_values_vector = [] (size_t size) noexcept {
        memory::disable_failure_guard dfg;
        std::vector<typename itraits::value_type> ret;
        ret.reserve(size);
        return ret;
    };
    std::vector<typename itraits::value_type> ret =
            make_values_vector(iterator_range_estimate_vector_capacity(begin, end, typename itraits::iterator_category()));
    // Important to invoke the *begin here, in case it's a function iterator,
    // so we launch all computation in parallel.
    std::move(begin, end, std::back_inserter(ret));
    return complete_when_all<ResolvedVectorTransform>(std::move(ret), ret.begin());
}

}
/// \endcond

/// Wait for many futures to complete, capturing possible errors (iterator version).
///
/// Given a range of futures as input, wait for all of them
/// to resolve (either successfully or with an exception), and return
/// them as a \c std::vector so individual values or exceptions can be examined.
///
/// \param begin an \c InputIterator designating the beginning of the range of futures
/// \param end an \c InputIterator designating the end of the range of futures
/// \return an \c std::vector<> of all the futures in the input; when
///         ready, all contained futures will be ready as well.
template <typename FutureIterator>
SEASTAR_CONCEPT( requires requires (FutureIterator i) { { *i++ }; requires is_future<std::remove_reference_t<decltype(*i)>>::value; } )
inline
future<std::vector<typename std::iterator_traits<FutureIterator>::value_type>>
when_all(FutureIterator begin, FutureIterator end) noexcept {
    namespace si = internal;
    using itraits = std::iterator_traits<FutureIterator>;
    using result_transform = si::identity_futures_vector<typename itraits::value_type>;
    try {
        return si::do_when_all<result_transform>(std::move(begin), std::move(end));
    } catch (...) {
        return result_transform::current_exception_as_future();
    }
}

template <typename T, bool IsFuture>
struct reducer_with_get_traits;

template <typename T>
struct reducer_with_get_traits<T, false> {
    using result_type = decltype(std::declval<T>().get());
    using future_type = future<result_type>;
    static future_type maybe_call_get(future<> f, lw_shared_ptr<T> r) {
        return f.then([r = std::move(r)] () mutable {
            return make_ready_future<result_type>(std::move(*r).get());
        });
    }
};

template <typename T>
struct reducer_with_get_traits<T, true> {
    using future_type = decltype(std::declval<T>().get());
    static future_type maybe_call_get(future<> f, lw_shared_ptr<T> r) {
        return f.then([r = std::move(r)] () mutable {
            return r->get();
        }).then_wrapped([r] (future_type f) {
            return f;
        });
    }
};

template <typename T, typename V = void>
struct reducer_traits {
    using future_type = future<>;
    static future_type maybe_call_get(future<> f, lw_shared_ptr<T> r) {
        return f.then([r = std::move(r)] {});
    }
};

template <typename T>
struct reducer_traits<T, decltype(std::declval<T>().get(), void())> : public reducer_with_get_traits<T, is_future<std::result_of_t<decltype(&T::get)(T)>>::value> {};

// @Mapper is a callable which transforms values from the iterator range
// into a future<T>. @Reducer is an object which can be called with T as
// parameter and yields a future<>. It may have a get() method which returns
// a value of type U which holds the result of reduction. This value is wrapped
// in a future and returned by this function. If the reducer has no get() method
// then this function returns future<>.
//
// TODO: specialize for non-deferring reducer
template <typename Iterator, typename Mapper, typename Reducer>
inline
auto
map_reduce(Iterator begin, Iterator end, Mapper&& mapper, Reducer&& r)
    -> typename reducer_traits<Reducer>::future_type
{
    auto r_ptr = make_lw_shared(std::forward<Reducer>(r));
    future<> ret = make_ready_future<>();
    while (begin != end) {
        ret = futurize_invoke(mapper, *begin++).then_wrapped([ret = std::move(ret), r_ptr] (auto f) mutable {
            return ret.then_wrapped([f = std::move(f), r_ptr] (auto rf) mutable {
                if (rf.failed()) {
                    f.ignore_ready_future();
                    return std::move(rf);
                } else {
                    return futurize_apply(*r_ptr, std::move(f.get()));
                }
            });
        });
    }
    return reducer_traits<Reducer>::maybe_call_get(std::move(ret), r_ptr);
}

/// Asynchronous map/reduce transformation.
///
/// Given a range of objects, an asynchronous unary function
/// operating on these objects, an initial value, and a
/// binary function for reducing, map_reduce() will
/// transform each object in the range, then invoke
/// the the reducing function with the result.
///
/// Example:
///
/// Calculate the total size of several files:
///
/// \code
///  map_reduce(files.begin(), files.end(),
///             std::mem_fn(file::size),
///             size_t(0),
///             std::plus<size_t>())
/// \endcode
///
/// Requirements:
///    - Iterator: an InputIterator.
///    - Mapper: unary function taking Iterator::value_type and producing a future<...>.
///    - Initial: any value type
///    - Reduce: a binary function taking two Initial values and returning an Initial
///
/// Return type:
///    - future<Initial>
///
/// \param begin beginning of object range to operate on
/// \param end end of object range to operate on
/// \param mapper map function to call on each object, returning a future
/// \param initial initial input value to reduce function
/// \param reduce binary function for merging two result values from \c mapper
///
/// \return equivalent to \c reduce(reduce(initial, mapper(obj0)), mapper(obj1)) ...
template <typename Iterator, typename Mapper, typename Initial, typename Reduce>
SEASTAR_CONCEPT( requires requires (Iterator i, Mapper mapper, Initial initial, Reduce reduce) {
     *i++;
     { i != i} -> std::convertible_to<bool>;
     mapper(*i);
     requires is_future<decltype(mapper(*i))>::value;
     { reduce(std::move(initial), mapper(*i).get0()) } -> std::convertible_to<Initial>;
} )
inline
future<Initial>
map_reduce(Iterator begin, Iterator end, Mapper&& mapper, Initial initial, Reduce reduce) {
    struct state {
        Initial result;
        Reduce reduce;
    };
    auto s = make_lw_shared(state{std::move(initial), std::move(reduce)});
    future<> ret = make_ready_future<>();
    while (begin != end) {
        ret = futurize_invoke(mapper, *begin++).then_wrapped([s = s.get(), ret = std::move(ret)] (auto f) mutable {
            try {
                s->result = s->reduce(std::move(s->result), std::move(f.get0()));
                return std::move(ret);
            } catch (...) {
                return std::move(ret).then_wrapped([ex = std::current_exception()] (auto f) {
                    f.ignore_ready_future();
                    return make_exception_future<>(ex);
                });
            }
        });
    }
    return ret.then([s] {
        return make_ready_future<Initial>(std::move(s->result));
    });
}

/// Asynchronous map/reduce transformation (range version).
///
/// Given a range of objects, an asynchronous unary function
/// operating on these objects, an initial value, and a
/// binary function for reducing, map_reduce() will
/// transform each object in the range, then invoke
/// the the reducing function with the result.
///
/// Example:
///
/// Calculate the total size of several files:
///
/// \code
///  std::vector<file> files = ...;
///  map_reduce(files,
///             std::mem_fn(file::size),
///             size_t(0),
///             std::plus<size_t>())
/// \endcode
///
/// Requirements:
///    - Iterator: an InputIterator.
///    - Mapper: unary function taking Iterator::value_type and producing a future<...>.
///    - Initial: any value type
///    - Reduce: a binary function taking two Initial values and returning an Initial
///
/// Return type:
///    - future<Initial>
///
/// \param range object range to operate on
/// \param mapper map function to call on each object, returning a future
/// \param initial initial input value to reduce function
/// \param reduce binary function for merging two result values from \c mapper
///
/// \return equivalent to \c reduce(reduce(initial, mapper(obj0)), mapper(obj1)) ...
template <typename Range, typename Mapper, typename Initial, typename Reduce>
SEASTAR_CONCEPT( requires requires (Range range, Mapper mapper, Initial initial, Reduce reduce) {
     std::begin(range);
     std::end(range);
     mapper(*std::begin(range));
     requires is_future<std::remove_reference_t<decltype(mapper(*std::begin(range)))>>::value;
     { reduce(std::move(initial), mapper(*std::begin(range)).get0()) } -> std::convertible_to<Initial>;
} )
inline
future<Initial>
map_reduce(Range&& range, Mapper&& mapper, Initial initial, Reduce reduce) {
    return map_reduce(std::begin(range), std::end(range), std::forward<Mapper>(mapper),
            std::move(initial), std::move(reduce));
}

// Implements @Reducer concept. Calculates the result by
// adding elements to the accumulator.
template <typename Result, typename Addend = Result>
class adder {
private:
    Result _result;
public:
    future<> operator()(const Addend& value) {
        _result += value;
        return make_ready_future<>();
    }
    Result get() && {
        return std::move(_result);
    }
};

inline
future<> now() {
    return make_ready_future<>();
}

// Returns a future which is not ready but is scheduled to resolve soon.
future<> later() noexcept;

class timed_out_error : public std::exception {
public:
    virtual const char* what() const noexcept {
        return "timedout";
    }
};

struct default_timeout_exception_factory {
    static auto timeout() {
        return timed_out_error();
    }
};

/// \brief Wait for either a future, or a timeout, whichever comes first
///
/// When timeout is reached the returned future resolves with an exception
/// produced by ExceptionFactory::timeout(). By default it is \ref timed_out_error exception.
///
/// Note that timing out doesn't cancel any tasks associated with the original future.
/// It also doesn't cancel the callback registerred on it.
///
/// \param f future to wait for
/// \param timeout time point after which the returned future should be failed
///
/// \return a future which will be either resolved with f or a timeout exception
template<typename ExceptionFactory = default_timeout_exception_factory, typename Clock, typename Duration, typename... T>
future<T...> with_timeout(std::chrono::time_point<Clock, Duration> timeout, future<T...> f) {
    if (f.available()) {
        return f;
    }
    auto pr = std::make_unique<promise<T...>>();
    auto result = pr->get_future();
    timer<Clock> timer([&pr = *pr] {
        pr.set_exception(std::make_exception_ptr(ExceptionFactory::timeout()));
    });
    timer.arm(timeout);
    // Future is returned indirectly.
    (void)f.then_wrapped([pr = std::move(pr), timer = std::move(timer)] (auto&& f) mutable {
        if (timer.cancel()) {
            f.forward_to(std::move(*pr));
        } else {
            f.ignore_ready_future();
        }
    });
    return result;
}

namespace internal {

template<typename Future>
struct future_has_value {
    enum {
        value = !std::is_same<std::decay_t<Future>, future<>>::value
    };
};

template<typename Tuple>
struct tuple_to_future;

template<typename... Elements>
struct tuple_to_future<std::tuple<Elements...>> {
    using type = future<Elements...>;
    using promise_type = promise<Elements...>;

    static auto make_ready(std::tuple<Elements...> t) noexcept {
        auto create_future = [] (auto&&... args) {
            return make_ready_future<Elements...>(std::move(args)...);
        };
        return apply(create_future, std::move(t));
    }

    static auto make_failed(std::exception_ptr excp) noexcept {
        return seastar::make_exception_future<Elements...>(std::move(excp));
    }
};

template<typename... Futures>
class extract_values_from_futures_tuple {
    static auto transform(std::tuple<Futures...> futures) noexcept {
        auto prepare_result = [] (auto futures) noexcept {
            auto fs = tuple_filter_by_type<internal::future_has_value>(std::move(futures));
            return tuple_map(std::move(fs), [] (auto&& e) {
                return internal::untuple(e.get());
            });
        };

        using tuple_futurizer = internal::tuple_to_future<decltype(prepare_result(std::move(futures)))>;

        std::exception_ptr excp;
        tuple_for_each(futures, [&excp] (auto& f) {
            if (!excp) {
                if (f.failed()) {
                    excp = f.get_exception();
                }
            } else {
                f.ignore_ready_future();
            }
        });
        if (excp) {
            return tuple_futurizer::make_failed(std::move(excp));
        }

        return tuple_futurizer::make_ready(prepare_result(std::move(futures)));
    }
public:
    using future_type = decltype(transform(std::declval<std::tuple<Futures...>>()));
    using promise_type = typename future_type::promise_type;

    static void set_promise(promise_type& p, std::tuple<Futures...> tuple) {
        transform(std::move(tuple)).forward_to(std::move(p));
    }

    static future_type make_ready_future(std::tuple<Futures...> tuple) noexcept {
        return transform(std::move(tuple));
    }

    static future_type current_exception_as_future() noexcept {
        future_type (*type_deduct)() = current_exception_as_future;
        return type_deduct();
    }
};

template<typename Future>
struct extract_values_from_futures_vector {
    using value_type = decltype(untuple(std::declval<typename Future::value_type>()));

    using future_type = future<std::vector<value_type>>;

    static future_type run(std::vector<Future> futures) noexcept {
        auto make_values_vector = [] (size_t size) noexcept {
            memory::disable_failure_guard dfg;
            std::vector<value_type> values;
            values.reserve(size);
            return values;
        };
        std::vector<value_type> values = make_values_vector(futures.size());

        std::exception_ptr excp;
        for (auto&& f : futures) {
            if (!excp) {
                if (f.failed()) {
                    excp = f.get_exception();
                } else {
                    values.emplace_back(untuple(f.get()));
                }
            } else {
                f.ignore_ready_future();
            }
        }
        if (excp) {
            return seastar::make_exception_future<std::vector<value_type>>(std::move(excp));
        }
        return make_ready_future<std::vector<value_type>>(std::move(values));
    }

    static future_type current_exception_as_future() noexcept {
        return seastar::current_exception_as_future<std::vector<value_type>>();
    }
};

template<>
struct extract_values_from_futures_vector<future<>> {
    using future_type = future<>;

    static future_type run(std::vector<future<>> futures) noexcept {
        std::exception_ptr excp;
        for (auto&& f : futures) {
            if (!excp) {
                if (f.failed()) {
                    excp = f.get_exception();
                }
            } else {
                f.ignore_ready_future();
            }
        }
        if (excp) {
            return seastar::make_exception_future<>(std::move(excp));
        }
        return make_ready_future<>();
    }

    static future_type current_exception_as_future() noexcept {
        return seastar::current_exception_as_future<>();
    }
};

template<typename... Futures>
SEASTAR_CONCEPT( requires seastar::AllAreFutures<Futures...> )
inline auto when_all_succeed_impl(Futures&&... futures) noexcept {
    using state = when_all_state<extract_values_from_futures_tuple<Futures...>, Futures...>;
    return state::wait_all(std::forward<Futures>(futures)...);
}

} // namespace internal

/// Wait for many futures to complete (variadic version).
///
/// Each future can be passed directly, or a function that returns a
/// future can be given instead.
///
/// If any function throws, or if the returned future fails, one of
/// the exceptions is returned by this function as a failed future.
///
/// \param fut_or_funcs futures or functions that return futures
/// \return future containing values of futures returned by funcs
template <typename... FutOrFuncs>
inline auto when_all_succeed(FutOrFuncs&&... fut_or_funcs) noexcept {
    return internal::when_all_succeed_impl(futurize_invoke_if_func(std::forward<FutOrFuncs>(fut_or_funcs))...);
}

/// Wait for many futures to complete (iterator version).
///
/// Given a range of futures as input, wait for all of them
/// to resolve, and return a future containing a vector of values of the
/// original futures.
/// In case any of the given futures fails one of the exceptions is returned
/// by this function as a failed future.
/// \param begin an \c InputIterator designating the beginning of the range of futures
/// \param end an \c InputIterator designating the end of the range of futures
/// \return an \c std::vector<> of all the valus in the input
template <typename FutureIterator, typename = typename std::iterator_traits<FutureIterator>::value_type>
SEASTAR_CONCEPT( requires requires (FutureIterator i) {
     *i++;
     { i != i } -> std::convertible_to<bool>;
     requires is_future<std::remove_reference_t<decltype(*i)>>::value;
} )
inline auto
when_all_succeed(FutureIterator begin, FutureIterator end) noexcept {
    using itraits = std::iterator_traits<FutureIterator>;
    using result_transform = internal::extract_values_from_futures_vector<typename itraits::value_type>;
    try {
        return internal::do_when_all<result_transform>(std::move(begin), std::move(end));
    } catch (...) {
        return result_transform::current_exception_as_future();
    }
}

}

/// @}
