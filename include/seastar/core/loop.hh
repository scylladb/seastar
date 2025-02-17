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
 * Copyright (C) 2020 ScyllaDB.
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <cassert>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>
#endif
#include <seastar/core/future.hh>
#include <seastar/core/task.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/bool_class.hh>
#include <seastar/util/modules.hh>
#include <seastar/core/semaphore.hh>

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

/// \addtogroup future-util
/// @{

// The AsyncAction concept represents an action which can complete later than
// the actual function invocation. It is represented by a function which
// returns a future which resolves when the action is done.

struct stop_iteration_tag { };
using stop_iteration = bool_class<stop_iteration_tag>;

namespace internal {

template <typename AsyncAction>
class repeater final : public continuation_base<stop_iteration> {
    promise<> _promise;
    AsyncAction _action;
public:
    explicit repeater(AsyncAction&& action) : _action(std::move(action)) {}
    future<> get_future() { return _promise.get_future(); }
    task* waiting_task() noexcept override { return _promise.waiting_task(); }
    virtual void run_and_dispose() noexcept override {
        if (_state.failed()) {
            _promise.set_exception(std::move(_state).get_exception());
            delete this;
            return;
        } else {
            if (_state.get() == stop_iteration::yes) {
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
                    internal::set_callback(std::move(f), this);
                    return;
                }
                if (f.get() == stop_iteration::yes) {
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

} // namespace internal

// Delete these overloads so that the actual implementation can use a
// universal reference but still reject lvalue references.
template<typename AsyncAction>
future<> repeat(const AsyncAction& action) noexcept = delete;
template<typename AsyncAction>
future<> repeat(AsyncAction& action) noexcept = delete;

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
requires std::is_invocable_r_v<stop_iteration, AsyncAction> || std::is_invocable_r_v<future<stop_iteration>, AsyncAction>
inline
future<> repeat(AsyncAction&& action) noexcept {
    using futurator = futurize<std::invoke_result_t<AsyncAction>>;
    static_assert(std::is_same_v<future<stop_iteration>, typename futurator::type>, "bad AsyncAction signature");
    for (;;) {
        // Do not type-erase here in case this is a short repeat()
        auto f = futurator::invoke(action);

        if (!f.available() || f.failed() || need_preempt()) {
            return [&] () noexcept {
                memory::scoped_critical_alloc_section _;
                auto repeater = new internal::repeater<AsyncAction>(std::move(action));
                auto ret = repeater->get_future();
                internal::set_callback(std::move(f), repeater);
                return ret;
            }();
        }

        if (f.get() == stop_iteration::yes) {
            return make_ready_future<>();
        }
    }
}

/// \cond internal

template <typename T>
struct repeat_until_value_type_helper;

/// Type helper for repeat_until_value()
template <typename T>
struct repeat_until_value_type_helper<future<std::optional<T>>> {
    /// The type of the value we are computing
    using value_type = T;
    /// Type used by \c AsyncAction while looping
    using optional_type = std::optional<T>;
    /// Return type of repeat_until_value()
    using future_type = future<value_type>;
};

/// Return value of repeat_until_value()
template <typename AsyncAction>
using repeat_until_value_return_type
        = typename repeat_until_value_type_helper<typename futurize<std::invoke_result_t<AsyncAction>>::type>::future_type;

/// \endcond

namespace internal {

template <typename AsyncAction, typename T>
class repeat_until_value_state final : public continuation_base<std::optional<T>> {
    promise<T> _promise;
    AsyncAction _action;
public:
    explicit repeat_until_value_state(AsyncAction action) : _action(std::move(action)) {}
    repeat_until_value_state(std::optional<T> st, AsyncAction action) : repeat_until_value_state(std::move(action)) {
        this->_state.set(std::move(st));
    }
    future<T> get_future() { return _promise.get_future(); }
    task* waiting_task() noexcept override { return _promise.waiting_task(); }
    virtual void run_and_dispose() noexcept override {
        if (this->_state.failed()) {
            _promise.set_exception(std::move(this->_state).get_exception());
            delete this;
            return;
        } else {
            auto v = std::move(this->_state).get();
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
                    internal::set_callback(std::move(f), this);
                    return;
                }
                auto ret = f.get();
                if (ret) {
                    _promise.set_value(std::move(*ret));
                    delete this;
                    return;
                }
            } while (!need_preempt());
        } catch (...) {
            _promise.set_exception(std::current_exception());
            delete this;
            return;
        }
        this->_state.set(std::nullopt);
        schedule(this);
    }
};

} // namespace internal

/// Invokes given action until it fails or the function requests iteration to stop by returning
/// an engaged \c future<std::optional<T>> or std::optional<T>.  The value is extracted
/// from the \c optional, and returned, as a future, from repeat_until_value().
///
/// \param action a callable taking no arguments, returning a future<std::optional<T>>
///               or std::optional<T>.  Will be called again as soon as the future
///               resolves, unless the future fails, action throws, or it resolves with
///               an engaged \c optional.  If \c action is an r-value it can be moved
///               in the middle of iteration.
/// \return a ready future if we stopped successfully, or a failed future if
///         a call to to \c action failed.  The \c optional's value is returned.
template<typename AsyncAction>
requires requires (AsyncAction aa) {
    bool(futurize_invoke(aa).get());
    futurize_invoke(aa).get().value();
}
repeat_until_value_return_type<AsyncAction>
repeat_until_value(AsyncAction action) noexcept {
    using futurator = futurize<std::invoke_result_t<AsyncAction>>;
    using type_helper = repeat_until_value_type_helper<typename futurator::type>;
    // the "T" in the documentation
    using value_type = typename type_helper::value_type;
    using optional_type = typename type_helper::optional_type;
    do {
        auto f = futurator::invoke(action);

        if (!f.available()) {
          return [&] () noexcept {
            memory::scoped_critical_alloc_section _;
            auto state = new internal::repeat_until_value_state<AsyncAction, value_type>(std::move(action));
            auto ret = state->get_future();
            internal::set_callback(std::move(f), state);
            return ret;
          }();
        }

        if (f.failed()) {
            return make_exception_future<value_type>(f.get_exception());
        }

        optional_type&& optional = std::move(f).get();
        if (optional) {
            return make_ready_future<value_type>(std::move(optional.value()));
        }
    } while (!need_preempt());

    try {
        auto state = new internal::repeat_until_value_state<AsyncAction, value_type>(std::nullopt, std::move(action));
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
    task* waiting_task() noexcept override { return _promise.waiting_task(); }
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
                    internal::set_callback(std::move(f), this);
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

} // namespace internal

/// Invokes given action until it fails or given condition evaluates to true or fails.
///
/// \param stop_cond a callable taking no arguments, returning a boolean that
///                  evalutes to true when you don't want to call \c action
///                  any longer. If \c stop_cond fails, the exception is propagated
//                   in the returned future.
/// \param action a callable taking no arguments, returning a future<>.  Will
///               be called again as soon as the future resolves, unless the
///               future fails, or \c stop_cond returns \c true or fails.
/// \return a ready future if we stopped successfully, or a failed future if
///         a call to to \c action or a call to \c stop_cond failed.
template<typename AsyncAction, typename StopCondition>
requires std::is_invocable_r_v<bool, StopCondition> && std::is_invocable_r_v<future<>, AsyncAction>
inline
future<> do_until(StopCondition stop_cond, AsyncAction action) noexcept {
    using namespace internal;
    for (;;) {
      try {
        if (stop_cond()) {
            return make_ready_future<>();
        }
      } catch (...) {
        return current_exception_as_future();
      }
        auto f = futurize_invoke(action);
        if (f.failed()) {
            return f;
        }
        if (!f.available() || need_preempt()) {
            return [&] () noexcept {
                memory::scoped_critical_alloc_section _;
                auto task = new do_until_state<StopCondition, AsyncAction>(std::move(stop_cond), std::move(action));
                auto ret = task->get_future();
                internal::set_callback(std::move(f), task);
                return ret;
            }();
        }
    }
}

/// Invoke given action until it fails.
///
/// Calls \c action repeatedly until it returns a failed future.
///
/// \param action a callable taking no arguments, returning a \c future<>
///        that becomes ready when you wish it to be called again.
/// \return a future<> that will resolve to the first failure of \c action
template<typename AsyncAction>
requires std::is_invocable_r_v<future<>, AsyncAction>
inline
future<> keep_doing(AsyncAction action) noexcept {
    return repeat([action = std::move(action)] () mutable {
        return action().then([] {
            return stop_iteration::no;
        });
    });
}

namespace internal {
template <typename Iterator, class Sentinel, typename AsyncAction>
class do_for_each_state final : public continuation_base<> {
    Iterator _begin;
    Sentinel _end;
    AsyncAction _action;
    promise<> _pr;

public:
    do_for_each_state(Iterator begin, Sentinel end, AsyncAction action, future<>&& first_unavailable)
        : _begin(std::move(begin)), _end(std::move(end)), _action(std::move(action)) {
        internal::set_callback(std::move(first_unavailable), this);
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
                internal::set_callback(std::move(f), this);
                zis.release();
                return;
            }
        }
        _pr.set_value();
    }
    task* waiting_task() noexcept override {
        return _pr.waiting_task();
    }
    future<> get_future() {
        return _pr.get_future();
    }
};

template<typename Iterator, typename Sentinel, typename AsyncAction>
inline
future<> do_for_each_impl(Iterator begin, Sentinel end, AsyncAction action) {
    while (begin != end) {
        auto f = futurize_invoke(action, *begin++);
        if (f.failed()) {
            return f;
        }
        if (!f.available() || need_preempt()) {
            auto* s = new internal::do_for_each_state<Iterator, Sentinel, AsyncAction>{
                std::move(begin), std::move(end), std::move(action), std::move(f)};
            return s->get_future();
        }
    }
    return make_ready_future<>();
}
} // namespace internal

/// \addtogroup future-util

/// \brief Call a function for each item in a range, sequentially (iterator version).
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
template<typename Iterator, typename Sentinel, typename AsyncAction>
requires (
    requires (Iterator i, AsyncAction aa) {
        { futurize_invoke(aa, *i) } -> std::same_as<future<>>;
    } &&
    (std::same_as<Sentinel, Iterator> || std::sentinel_for<Sentinel, Iterator>)
)
inline
future<> do_for_each(Iterator begin, Sentinel end, AsyncAction action) noexcept {
    try {
        return internal::do_for_each_impl(std::move(begin), std::move(end), std::move(action));
    } catch (...) {
        return current_exception_as_future();
    }
}

/// \brief Call a function for each item in a range, sequentially (range version).
///
/// For each item in a range, call a function, waiting for the previous
/// invocation to complete before calling the next one.
///
/// \param c an \c Range object designating input range
/// \param action a callable, taking a reference to objects from the range
///               as a parameter, and returning a \c future<> that resolves
///               when it is acceptable to process the next item.
/// \return a ready future on success, or the first failed future if
///         \c action failed.
template<typename Range, typename AsyncAction>
requires requires (Range c, AsyncAction aa) {
    { futurize_invoke(aa, *std::begin(c)) } -> std::same_as<future<>>;
    std::end(c);
}
inline
future<> do_for_each(Range& c, AsyncAction action) noexcept {
    try {
        return internal::do_for_each_impl(std::begin(c), std::end(c), std::move(action));
    } catch (...) {
        return current_exception_as_future();
    }
}

namespace internal {

template <typename T, typename = void>
struct has_iterator_category : std::false_type {};

template <typename T>
struct has_iterator_category<T, std::void_t<typename std::iterator_traits<T>::iterator_category >> : std::true_type {};

template <typename Iterator, typename Sentinel>
inline
size_t
iterator_range_estimate_vector_capacity(Iterator begin, Sentinel end) {
    if constexpr (std::forward_iterator<Iterator> &&
                  std::forward_iterator<Sentinel>) {
        return std::ranges::distance(begin, end);
    } else if constexpr (std::random_access_iterator<Iterator> &&
                         std::random_access_iterator<Sentinel>) {
        return std::ranges::distance(begin, end);
    } else {
        // For InputIterators we can't estimate needed capacity
        return 0;
    }
}

} // namespace internal

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
    task* waiting_task() noexcept override { return _result.waiting_task(); }
public:
    parallel_for_each_state(size_t n);
    void add_future(future<>&& f);
    future<> get_future();
};

/// \endcond

/// \brief Run tasks in parallel (iterator version).
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
/// \note parallel_for_each() schedules all invocations of \c func on the
///       current shard. If you want to run a function on all shards in parallel,
///       have a look at \ref smp::invoke_on_all() instead.
template <typename Iterator, typename Sentinel, typename Func>
requires (requires (Func f, Iterator i) { { f(*i) } -> std::same_as<future<>>; { i++ }; } && (std::same_as<Sentinel, Iterator> || std::sentinel_for<Sentinel, Iterator>))
// We use a conjunction with std::same_as<Sentinel, Iterator> because std::sentinel_for requires Sentinel to be semiregular,
// which implies that it requires Sentinel to be default-constructible, which is unnecessarily strict in below's context and could
// break legacy code, for which it holds that Sentinel equals Iterator.
inline
future<>
parallel_for_each(Iterator begin, Sentinel end, Func&& func) noexcept {
    parallel_for_each_state* s = nullptr;
    // Process all elements, giving each future the following treatment:
    //   - available, not failed: do nothing
    //   - available, failed: collect exception in ex
    //   - not available: collect in s (allocating it if needed)
    while (begin != end) {
        auto f = futurize_invoke(std::forward<Func>(func), *begin);
        ++begin;
        memory::scoped_critical_alloc_section _;
        if (!f.available() || f.failed()) {
            if (!s) {
                size_t n{0U};
                if constexpr (internal::has_iterator_category<Iterator>::value) {
                    // We need if-constexpr here because there exist iterators for which std::iterator_traits
                    // does not have 'iterator_category' as member type
                    n = (internal::iterator_range_estimate_vector_capacity(begin, end) + 1);
                }
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

/// \brief Run tasks in parallel (range version).
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
/// \note parallel_for_each() schedules all invocations of \c func on the
///       current shard. If you want to run a function on all shards in parallel,
///       have a look at \ref smp::invoke_on_all() instead.

namespace internal {

template <typename Range, typename Func>
inline
future<>
parallel_for_each_impl(Range&& range, Func&& func) {
    return parallel_for_each(std::begin(range), std::end(range),
            std::forward<Func>(func));
}

} // namespace internal

template <typename Range, typename Func>
requires requires (Func f, Range r) {
    { f(*std::begin(r)) } -> std::same_as<future<>>;
    std::end(r);
}
inline
future<>
parallel_for_each(Range&& range, Func&& func) noexcept {
    auto impl = internal::parallel_for_each_impl<Range, Func>;
    return futurize_invoke(impl, std::forward<Range>(range), std::forward<Func>(func));
}

/// Run a maximum of \c max_concurrent tasks in parallel (iterator version).
///
/// Given a range [\c begin, \c end) of objects, run \c func on each \c *i in
/// the range, and return a future<> that resolves when all the functions
/// complete.  \c func should return a future<> that indicates when it is
/// complete.  Up to \c max_concurrent invocations are performed in parallel.
/// This does not allow the range to refer to stack objects. The caller
/// must ensure that the range outlives the call to max_concurrent_for_each
/// so it can be iterated in the background.
///
/// \param begin an \c InputIterator designating the beginning of the range
/// \param end an \c InputIterator designating the end of the range
/// \param max_concurrent maximum number of concurrent invocations of \c func, must be greater than zero.
/// \param func Function to invoke with each element in the range (returning
///             a \c future<>)
/// \return a \c future<> that resolves when all the function invocations
///         complete.  If one or more return an exception, the return value
///         contains one of the exceptions.
/// \note max_concurrent_for_each() schedules all invocations of \c func on the
///       current shard. If you want to run a function on all shards in parallel,
///       have a look at \ref smp::invoke_on_all() instead.
template <typename Iterator, typename Sentinel, typename Func>
requires (requires (Func f, Iterator i) { { f(*i) } -> std::same_as<future<>>; { ++i }; } && (std::same_as<Sentinel, Iterator> || std::sentinel_for<Sentinel, Iterator>) )
// We use a conjunction with std::same_as<Sentinel, Iterator> because std::sentinel_for requires Sentinel to be semiregular,
// which implies that it requires Sentinel to be default-constructible, which is unnecessarily strict in below's context and could
// break legacy code, for which it holds that Sentinel equals Iterator.
inline
future<>
max_concurrent_for_each(Iterator begin, Sentinel end, size_t max_concurrent, Func&& func) noexcept {
    struct state {
        Iterator begin;
        Sentinel end;
        Func func;
        size_t max_concurrent;
        semaphore sem;
        std::exception_ptr err;

        state(Iterator begin_, Sentinel end_, size_t max_concurrent_, Func func_)
            : begin(std::move(begin_))
            , end(std::move(end_))
            , func(std::move(func_))
            , max_concurrent(max_concurrent_)
            , sem(max_concurrent_)
            , err()
        { }
    };

    SEASTAR_ASSERT(max_concurrent > 0);

    try {
        return do_with(state(std::move(begin), std::move(end), max_concurrent, std::forward<Func>(func)), [] (state& s) {
            return do_until([&s] { return s.begin == s.end; }, [&s] {
                return s.sem.wait().then([&s] () mutable noexcept {
                    // Possibly run in background and signal _sem when the task is done.
                    // The background tasks are waited on using _sem.
                    (void)futurize_invoke(s.func, *s.begin).then_wrapped([&s] (future<> fut) {
                        if (fut.failed()) {
                            auto e = fut.get_exception();;
                            if (!s.err) {
                                s.err = std::move(e);
                            }
                        }
                        s.sem.signal();
                    });
                    ++s.begin;
                });
            }).then([&s] {
                // Wait for any background task to finish
                // and signal and semaphore
                return s.sem.wait(s.max_concurrent);
            }).then([&s] {
                if (!s.err) {
                    return make_ready_future<>();
                }
                return seastar::make_exception_future<>(std::move(s.err));
            });
        });
    } catch (...) {
        return current_exception_as_future();
    }
}

/// Run a maximum of \c max_concurrent tasks in parallel (range version).
///
/// Given a range of objects, run \c func on each \c *i in
/// the range, and return a future<> that resolves when all the functions
/// complete.  \c func should return a future<> that indicates when it is
/// complete.  Up to \c max_concurrent invocations are performed in parallel.
/// This does not allow the range to refer to stack objects. The caller
/// must ensure that the range outlives the call to max_concurrent_for_each
/// so it can be iterated in the background.
///
/// \param range a \c Range to be processed
/// \param max_concurrent maximum number of concurrent invocations of \c func, must be greater than zero.
/// \param func Function to invoke with each element in the range (returning
///             a \c future<>)
/// \return a \c future<> that resolves when all the function invocations
///         complete.  If one or more return an exception, the return value
///         contains one of the exceptions.
/// \note max_concurrent_for_each() schedules all invocations of \c func on the
///       current shard. If you want to run a function on all shards in parallel,
///       have a look at \ref smp::invoke_on_all() instead.
template <typename Range, typename Func>
requires requires (Func f, Range r) {
    { f(*std::begin(r)) } -> std::same_as<future<>>;
    std::end(r);
}
inline
future<>
max_concurrent_for_each(Range&& range, size_t max_concurrent, Func&& func) noexcept {
    try {
        return max_concurrent_for_each(std::begin(range), std::end(range), max_concurrent, std::forward<Func>(func));
    } catch (...) {
        return current_exception_as_future();
    }
}

/// @}

SEASTAR_MODULE_EXPORT_END

} // namespace seastar
