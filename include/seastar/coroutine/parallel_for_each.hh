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
 * Copyright (C) 2022-present ScyllaDB
 */

#pragma once

#include <ranges>

#include <boost/container/small_vector.hpp>

#include <seastar/core/loop.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>

namespace seastar::coroutine {

/// Invoke a function on all elements in a range in parallel and wait for all futures to complete in a coroutine.
///
/// `parallel_for_each` can be used to launch a function concurrently
/// on all elements in a given range and wait for all of them to complete.
/// Waiting is performend by `co_await` and returns a future.
///
/// If one or more of the function invocations resolve to an exception
/// then the one of the exceptions is re-thrown.
/// All of the futures are waited for, even in the case of exceptions.
///
/// Example
///
/// ```
/// future<int> sum_of_squares(std::vector<int> v) {
///     int sum = 0;
///     return co_await parallel_for_each(v, [&sum] (int& x) {
///         sum += x * x;
///     });
///     co_return sum;
/// };
/// ```
///
/// Safe for use with lambda coroutines.
///
/// \note parallel_for_each() schedules all invocations of \c func on the
///       current shard. If you want to run a function on all shards in parallel,
///       have a look at \ref smp::invoke_on_all() instead.
template <typename Func>
// constaints for Func are defined at the parallel_for_each constructor
class [[nodiscard("must co_await an parallel_for_each() object")]] parallel_for_each final : continuation_base<> {
    using coroutine_handle_t = std::coroutine_handle<void>;

    Func _func;
    boost::container::small_vector<future<>, 5> _futures;
    std::exception_ptr _ex;
    coroutine_handle_t _when_ready;
    task* _waiting_task = nullptr;

    // Consume futures in reverse order.
    // Since futures at the front are expected
    // to become ready before futures at the back,
    // therefore it is less likely we will have
    // to wait on them, after the back futures
    // become available.
    //
    // Return true iff all futures were consumed.
    bool consume_next() noexcept {
        while (!_futures.empty()) {
            auto& fut = _futures.back();
            if (!fut.available()) {
                return false;
            }
            if (fut.failed()) {
                _ex = fut.get_exception();
            }
            _futures.pop_back();
        }
        return true;
    }

    void set_callback() noexcept {
        // To reuse `this` as continuation_base<>
        // we must reset _state, to allow setting
        // it again.
        this->_state = {};
        seastar::internal::set_callback(std::move(_futures.back()), reinterpret_cast<continuation_base<>*>(this));
        _futures.pop_back();
    }

    void resume_or_set_callback() noexcept {
        if (consume_next()) {
            local_engine->set_current_task(_waiting_task);
            _when_ready.resume();
        } else {
            set_callback();
        }
    }

public:
    // clang 13.0.1 doesn't support subrange
    // so provide also a Iterator/Sentinel based constructor.
    // See https://github.com/llvm/llvm-project/issues/46091
    template <typename Iterator, typename Sentinel, typename Func1>
    requires (std::same_as<Sentinel, Iterator> || std::sentinel_for<Sentinel, Iterator>)
        && std::same_as<future<>, futurize_t<std::invoke_result_t<Func, typename std::iterator_traits<Iterator>::reference>>>
    explicit parallel_for_each(Iterator begin, Sentinel end, Func1&& func) noexcept
        : _func(std::forward<Func1>(func))
    {
        for (auto it = begin; it != end; ++it) {
            auto fut = futurize_invoke(_func, *it);
            if (fut.available()) {
                if (fut.failed()) {
                    _ex = fut.get_exception();
                }
            } else {
                memory::scoped_critical_alloc_section _;
                if (_futures.empty()) {
                    if constexpr (seastar::internal::has_iterator_category<Iterator>::value) {
                        auto n = seastar::internal::iterator_range_estimate_vector_capacity(it, end);
                        _futures.reserve(n);
                    }
                }
                _futures.push_back(std::move(fut));
            }
        }
    }

    template <std::ranges::range Range, typename Func1>
    requires std::invocable<Func, std::ranges::range_reference_t<Range>>
    explicit parallel_for_each(Range&& range, Func1&& func) noexcept
        : parallel_for_each(std::ranges::begin(range), std::ranges::end(range), std::forward<Func1>(func))
    { }

    bool await_ready() const noexcept {
        if (_futures.empty()) {
            return !_ex;
        }
        return false;
    }

    template<typename T>
    void await_suspend(std::coroutine_handle<T> h) {
        _when_ready = h;
        _waiting_task = &h.promise();
        resume_or_set_callback();
    }

    void await_resume() const {
        if (_ex) [[unlikely]] {
            std::rethrow_exception(std::move(_ex));
        }
    }

    virtual void run_and_dispose() noexcept override {
        if (this->_state.failed()) {
            _ex = std::move(this->_state).get_exception();
        }
        resume_or_set_callback();
    }

    virtual task* waiting_task() noexcept override {
        return _waiting_task;
    }
};

template <typename Iterator, typename Sentinel, typename Func>
requires (std::same_as<Sentinel, Iterator> || std::sentinel_for<Sentinel, Iterator>)
    && std::same_as<future<>, futurize_t<std::invoke_result_t<Func, typename std::iterator_traits<Iterator>::reference>>>
parallel_for_each(Iterator begin, Sentinel end, Func&& func) -> parallel_for_each<Func>;

template <std::ranges::range Range,
          std::invocable<std::ranges::range_reference_t<Range>> Func>
parallel_for_each(Range&& range, Func&& func) -> parallel_for_each<Func>;



}
