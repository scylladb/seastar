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
#include <seastar/core/future.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/tuple_utils.hh>
#include <seastar/util/critical_alloc_section.hh>
#include <seastar/util/modules.hh>
#include <cstddef>
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#endif

namespace seastar {

/// \addtogroup future-util
/// @{

namespace internal {

template<typename... Futures>
struct identity_futures_tuple {
    using future_type = future<std::tuple<Futures...>>;
    using promise_type = typename future_type::promise_type;

    static void set_promise(promise_type& p, std::tuple<Futures...> futures) {
        p.set_value(std::move(futures));
    }

    static future_type make_ready_future(std::tuple<Futures...> futures) noexcept {
        return seastar::make_ready_future<std::tuple<Futures...>>(std::move(futures));
    }

    static future_type current_exception_as_future() noexcept {
        return seastar::current_exception_as_future<std::tuple<Futures...>>();
    }
};

class when_all_state_base;

// If the future is ready, return true
// if the future is not ready, chain a continuation to it, and return false
using when_all_process_element_func = bool (*)(void* future, void* continuation, when_all_state_base* wasb) noexcept;

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
    virtual task* waiting_task() = 0;
    void complete_one() noexcept {
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
    void do_wait_all() noexcept {
        ++_nr_remain; // fake pending completion for complete_one()
        complete_one();
    }
    bool process_one(size_t idx) noexcept {
        auto p = _processors[idx];
        return p.func(p.future, _continuation, this);
    }
};

template <typename Future>
class when_all_state_component final : public continuation_base_from_future_t<Future> {
    when_all_state_base* _base;
    Future* _final_resting_place;
public:
    static bool process_element_func(void* future, void* continuation, when_all_state_base* wasb) noexcept {
        auto f = reinterpret_cast<Future*>(future);
        if (f->available()) {
            return true;
        } else {
            auto c = new (continuation) when_all_state_component(wasb, f);
            set_callback(std::move(*f), c);
            return false;
        }
    }
    when_all_state_component(when_all_state_base *base, Future* future) noexcept : _base(base), _final_resting_place(future) {}
    task* waiting_task() noexcept override { return _base->waiting_task(); }
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

template<typename ResolvedTupleTransform, typename... Futures>
class when_all_state : public when_all_state_base {
    static constexpr size_t nr = sizeof...(Futures);
    using type = std::tuple<Futures...>;
    type tuple;
    // We only schedule one continuation at a time, and store it in _cont.
    // This way, if while the future we wait for completes, some other futures
    // also complete, we won't need to schedule continuations for them.
    alignas(when_all_state_component<Futures>...) std::byte _cont[std::max({sizeof(when_all_state_component<Futures>)...})];
    when_all_process_element _processors[nr];
public:
    typename ResolvedTupleTransform::promise_type p;
    when_all_state(Futures&&... t) : when_all_state_base(nr, _processors, &_cont), tuple(std::make_tuple(std::move(t)...)) {
        init_element_processors(std::make_index_sequence<nr>());
    }
    virtual ~when_all_state() {
        ResolvedTupleTransform::set_promise(p, std::move(tuple));
    }
    task* waiting_task() noexcept override {
        return p.waiting_task();
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
        if ((futures.available() && ...)) {
            return ResolvedTupleTransform::make_ready_future(std::make_tuple(std::move(futures)...));
        }
        auto state = [&] () noexcept {
            memory::scoped_critical_alloc_section _;
            return new when_all_state(std::move(futures)...);
        }();
        auto ret = state->p.get_future();
        state->do_wait_all();
        return ret;
    }
};

} // namespace internal

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

template <typename... Futs>
concept AllAreFutures = impl::is_tuple_of_futures<std::tuple<Futs...>>::value;

template<typename Fut, std::enable_if_t<is_future<Fut>::value, int> = 0>
auto futurize_invoke_if_func(Fut&& fut) noexcept {
    return std::forward<Fut>(fut);
}

template<typename Func, std::enable_if_t<!is_future<Func>::value, int> = 0>
auto futurize_invoke_if_func(Func&& func) noexcept {
    return futurize_invoke(std::forward<Func>(func));
}
/// \endcond

namespace internal {

template <typename... Futs>
requires seastar::AllAreFutures<Futs...>
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
SEASTAR_MODULE_EXPORT
template <typename... FutOrFuncs>
inline auto when_all(FutOrFuncs&&... fut_or_funcs) noexcept {
    return internal::when_all_impl(futurize_invoke_if_func(std::forward<FutOrFuncs>(fut_or_funcs))...);
}

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

template<typename ResolvedVectorTransform, typename FutureIterator, typename Sentinel>
inline auto
do_when_all(FutureIterator begin, Sentinel end) noexcept {
    using itraits = std::iterator_traits<FutureIterator>;
    auto make_values_vector = [] (size_t size) noexcept {
        memory::scoped_critical_alloc_section _;
        std::vector<typename itraits::value_type> ret;
        ret.reserve(size);
        return ret;
    };
    std::vector<typename itraits::value_type> ret =
            make_values_vector(iterator_range_estimate_vector_capacity(begin, end));
    // Important to invoke the *begin here, in case it's a function iterator,
    // so we launch all computation in parallel.
    std::ranges::move(begin, end, std::back_inserter(ret));
    return complete_when_all<ResolvedVectorTransform>(std::move(ret), ret.begin());
}

} // namespace internal

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
SEASTAR_MODULE_EXPORT
template <typename FutureIterator>
requires requires (FutureIterator i) { { *i++ }; requires is_future<std::remove_reference_t<decltype(*i)>>::value; }
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

namespace internal {

template<typename Future>
struct future_has_value {
    enum {
        value = !std::is_same_v<std::decay_t<Future>, future<>>
    };
};

template<typename Tuple>
struct tuple_to_future;

template<typename... Elements>
struct tuple_to_future<std::tuple<Elements...>> {
    using value_type = std::tuple<Elements...>;
    using type = future<value_type>;
    using promise_type = promise<value_type>;

    // Elements... all come from futures, so we know they are nothrow move
    // constructible. `future` also has a static assertion to that effect.

    static auto make_ready(std::tuple<Elements...> t) noexcept {
        return make_ready_future<value_type>(value_type(std::move(t)));
    }

    static auto make_failed(std::exception_ptr excp) noexcept {
        return seastar::make_exception_future<value_type>(std::move(excp));
    }
};

template<typename... Futures>
class extract_values_from_futures_tuple {
    static auto transform(std::tuple<Futures...> futures) noexcept {
        auto prepare_result = [] (auto futures) noexcept {
            auto fs = tuple_filter_by_type<internal::future_has_value>(std::move(futures));
            return tuple_map(std::move(fs), [] (auto&& e) {
                return e.get();
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
    using value_type = decltype(untuple(std::declval<typename Future::tuple_type>()));

    using future_type = future<std::vector<value_type>>;

    static future_type run(std::vector<Future> futures) noexcept {
        auto make_values_vector = [] (size_t size) noexcept {
            memory::scoped_critical_alloc_section _;
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
                    values.emplace_back(f.get());
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
requires seastar::AllAreFutures<Futures...>
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
SEASTAR_MODULE_EXPORT
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
SEASTAR_MODULE_EXPORT
template <typename FutureIterator, typename Sentinel, typename = typename std::iterator_traits<FutureIterator>::value_type>
requires requires (FutureIterator i) {
     *i++;
     { i != i } -> std::convertible_to<bool>;
     requires is_future<std::remove_reference_t<decltype(*i)>>::value;
}
inline auto
when_all_succeed(FutureIterator begin, Sentinel end) noexcept {
    using itraits = std::iterator_traits<FutureIterator>;
    using result_transform = internal::extract_values_from_futures_vector<typename itraits::value_type>;
    try {
        return internal::do_when_all<result_transform>(std::move(begin), std::move(end));
    } catch (...) {
        return result_transform::current_exception_as_future();
    }
}


/// Wait for many futures to complete (vector version).
///
/// Given a vector of futures as input, wait for all of them
/// to resolve, and return a future containing a vector of values of the
/// original futures.
///
/// In case any of the given futures fails one of the exceptions is returned
/// by this function as a failed future.
///
/// \param futures a \c std::vector containing the futures to wait for.
/// \return an \c std::vector<> of all the values in the input
SEASTAR_MODULE_EXPORT
template <typename T>
inline auto
when_all_succeed(std::vector<future<T>>&& futures) noexcept {
    using result_transform = internal::extract_values_from_futures_vector<future<T>>;
    try {
        return internal::complete_when_all<result_transform>(std::move(futures), futures.begin());
    } catch (...) {
        return result_transform::current_exception_as_future();
    }
}

/// @}

} // namespace seastar
