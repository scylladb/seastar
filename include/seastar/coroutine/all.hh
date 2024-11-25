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
 * Copyright (C) 2021-present ScyllaDB
 */

#pragma once

#include <cstddef>
#include <concepts>
#include <tuple>
#include <seastar/core/coroutine.hh>

namespace seastar::coroutine {

template <typename Future>
constexpr inline bool is_future_v = is_future<Future>::value;

template <typename Future>
concept future_type = is_future_v<Future>;

namespace internal {

// Given a bunch of futures, find the indexes of the ones that are not avoid
// and store them in member type `type` as an std::integer_sequence.
//
// `IndexSequence` and `current` are intermediates used for recursion.
template <typename IndexSequence, size_t current, typename... Futures>
struct index_sequence_for_non_void_futures_helper;

// Terminate recursion be returning the accumulated `IndexSequence`
template <typename IndexSequence, size_t current>
struct index_sequence_for_non_void_futures_helper<IndexSequence, current> {
    using type = IndexSequence;
};

// Process a future<T> by adding it to the current IndexSequence and recursing
template <size_t... Existing, size_t current, typename T, typename... Futures>
struct index_sequence_for_non_void_futures_helper<std::integer_sequence<size_t, Existing...>, current, future<T>, Futures...> {
    using type = typename index_sequence_for_non_void_futures_helper<std::integer_sequence<size_t, Existing..., current>, current + 1, Futures...>::type;
};

// Process a future<void> by ignoring it and recursing
template <size_t... Existing, size_t current, typename... Futures>
struct index_sequence_for_non_void_futures_helper<std::integer_sequence<size_t, Existing...>, current, future<>, Futures...> {
    using type = typename index_sequence_for_non_void_futures_helper<std::integer_sequence<size_t, Existing...>, current + 1, Futures...>::type;
};

// Simple interface for the above.
template <typename... Futures>
using index_sequence_for_non_void_futures = typename index_sequence_for_non_void_futures_helper<std::integer_sequence<size_t>, 0, Futures...>::type;

// Given a tuple of futures, return a tuple of the value types, excluding future<void>.
template <typename IndexSequence, typename FutureTuple>
struct value_tuple_for_non_void_futures_helper;

template <size_t... Idx, typename FutureTuple>
struct value_tuple_for_non_void_futures_helper<std::integer_sequence<size_t, Idx...>, FutureTuple> {
    using type = std::tuple<typename std::tuple_element_t<Idx, FutureTuple>::value_type...>;
};

// Simple interface for the above
template <typename... Futures>
using value_tuple_for_non_void_futures = typename value_tuple_for_non_void_futures_helper<index_sequence_for_non_void_futures<Futures...>, std::tuple<Futures...>>::type;

}

/// Wait for serveral futures to complete in a coroutine.
///
/// `all` can be used to launch several computations concurrently
/// and wait for all of them to complete. Computations are provided
/// as callable objects (typically lambda coroutines) that are invoked
/// by `all`. Waiting is performend by `co_await` and returns a tuple
/// of values, one for each non-void future.
///
/// If one or more of the function objects throws an exception, or if one
/// or more of the futures resolves to an exception, then the exception is
/// thrown. All of the futures are waited for, even in the case of exceptions.
/// If more than one exception is present, an arbitrary one is thrown.
///
/// Example
///
/// ```
/// future<int> add() {
///     auto [a, b] = co_await all(
///         [] () -> future<int> {
///             co_await sleep(1ms);
///             co_return 2;
///         },
///         [] () -> future<int> {
///             co_await sleep(1ms);
///             co_return 3;
///         }
///     );
///     co_return a + b;
/// };
/// ```
///
/// Safe for use with lambda coroutines.
template <typename... Futures>
requires (sizeof ...(Futures) > 0)
class [[nodiscard("must co_await an all() object")]] all {
    using tuple = std::tuple<Futures...>;
    using value_tuple = typename internal::value_tuple_for_non_void_futures<Futures...>;
    struct awaiter;
    template <size_t idx>
    struct intermediate_task final : continuation_base_from_future_t<std::tuple_element_t<idx, tuple>> {
        awaiter& container;
        explicit intermediate_task(awaiter& container) : container(container) {}
        virtual void run_and_dispose() noexcept {
            using value_type = typename std::tuple_element_t<idx, tuple>::value_type;
            if (__builtin_expect(this->_state.failed(), false)) {
                using futurator = futurize<std::tuple_element_t<idx, tuple>>;
                std::get<idx>(container.state._futures) = futurator::make_exception_future(std::move(this->_state).get_exception());
            } else {
                if constexpr (std::same_as<std::tuple_element_t<idx, tuple>, future<>>) {
                    std::get<idx>(container.state._futures) = make_ready_future<>();
                } else {
                    std::get<idx>(container.state._futures) = make_ready_future<value_type>(std::move(this->_state).get());
                }
            }
            awaiter& c = container;
            this->~intermediate_task();
            c.template process<idx+1>();
        }
    };
    template <typename IndexSequence>
    struct generate_aligned_union;
    template <size_t... idx>
    struct generate_aligned_union<std::integer_sequence<size_t, idx...>> {
        static constexpr std::size_t alignment_value = std::max({alignof(intermediate_task<idx>)...});
        using type = std::byte[std::max({sizeof(intermediate_task<idx>)...})];
    };
    using continuation_storage = generate_aligned_union<std::make_index_sequence<std::tuple_size_v<tuple>>>;
    using coroutine_handle_t = std::coroutine_handle<void>;
private:
    tuple _futures;
private:
    struct awaiter {
        all& state;
        alignas(continuation_storage::alignment_value) typename continuation_storage::type _continuation_storage;
        coroutine_handle_t when_ready;
        awaiter(all& state) : state(state) {}
        bool await_ready() const {
            return std::apply([] (const Futures&... futures) {
                return (... && futures.available());
            }, state._futures);
        }
        void await_suspend(coroutine_handle_t h) {
            when_ready = h;
            process<0>();
        }
        value_tuple await_resume() {
            std::apply([] (Futures&... futures) {
                std::exception_ptr e;
                // Call get_exception for every failed future, to avoid exceptional future
                // ignored warnings.
                (void)(..., (futures.failed() ? (e = futures.get_exception(), 0) : 0));
                if (e) {
                    std::rethrow_exception(std::move(e));
                }
            }, state._futures);
            // This immediately-invoked lambda is used to materialize the indexes
            // of non-void futures in the tuple.
            return [&] <size_t... Idx> (std::integer_sequence<size_t, Idx...>) {
                return value_tuple(std::get<Idx>(state._futures).get()...);
            } (internal::index_sequence_for_non_void_futures<Futures...>());
        }
        template <unsigned idx>
        void process() {
            if constexpr (idx == sizeof...(Futures)) {
                when_ready.resume();
            } else {
                if (!std::get<idx>(state._futures).available()) {
                    auto task = new (&_continuation_storage) intermediate_task<idx>(*this);
                    seastar::internal::set_callback(std::move(std::get<idx>(state._futures)), task);
                } else {
                    process<idx + 1>();
                }
            }
        }
    };
public:
    template <typename... Func>
    requires (... && std::invocable<Func>) && (... && future_type<std::invoke_result_t<Func>>)
    explicit all(Func&&... funcs)
            : _futures(futurize_invoke(funcs)...) {
    }
    awaiter operator co_await() { return awaiter{*this}; }
};

template <typename FirstFunc, typename... MoreFuncs>
explicit all(FirstFunc&&, MoreFuncs&&...) -> all<std::invoke_result_t<FirstFunc>,
                                                 std::invoke_result_t<MoreFuncs>...>;

}
