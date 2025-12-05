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
 * Copyright (C) 2025-present ScyllaDB
 */

#pragma once

#include <seastar/core/coroutine.hh>
#include <seastar/core/internal/current_task.hh>

namespace seastar::internal {

template <typename T, typename U>
void try_future_resume_or_destroy_coroutine(seastar::future<T>& fut, seastar::task& coroutine_task) {
    auto promise_ptr = static_cast<U*>(&coroutine_task);
    auto hndl = std::coroutine_handle<U>::from_promise(*promise_ptr);

    if (fut.failed()) {
        hndl.promise().set_exception(std::move(fut).get_exception());
        hndl.destroy();
    } else {
        set_current_task(promise_ptr);
        hndl.resume();
    }
}

template <bool CheckPreempt, typename T>
class [[nodiscard]] try_future_awaiter : public seastar::task {
    seastar::future<T> _future;
    void (*_resume_or_destroy)(seastar::future<T>&, seastar::task&){};
    seastar::task* _coroutine_task{};
    seastar::task* _waiting_task{};

public:
    explicit try_future_awaiter(seastar::future<T>&& f) noexcept : _future(std::move(f)) {}

    try_future_awaiter(const try_future_awaiter&) = delete;
    try_future_awaiter(try_future_awaiter&&) = delete;

    bool await_ready() const noexcept {
        // Will suspend+schedule for ready failed futures too.
        return _future.available() && !_future.failed() && (!CheckPreempt || !need_preempt());
    }

    template<typename U>
    void await_suspend(std::coroutine_handle<U> hndl) noexcept {
        _resume_or_destroy = try_future_resume_or_destroy_coroutine<T, U>;
        _coroutine_task = &hndl.promise();
        _waiting_task = hndl.promise().waiting_task();

        if (_future.available()) {
            execute_involving_handle_destruction_in_await_suspend(this);
        } else {
            _future.set_coroutine(*this);
        }
    }

    T await_resume() {
        if constexpr (std::is_void_v<T>) {
            _future.get();
        } else {
            return std::move(_future).get();
        }
    }

    virtual void run_and_dispose() noexcept final override {
        _resume_or_destroy(_future, *_coroutine_task);
    }

    virtual task* waiting_task() noexcept override {
        return _waiting_task;
    }
};

} // namespace seastar::internal

namespace seastar::coroutine {

/// \brief co_await:s a \ref future and returns the wrapped result if successful,
/// terminates the coroutine otherwise, propagating the exception directly to the
/// waiter.
///
/// If the future was successful, this is identical to co_await-ing the future
/// directly. If the future failed, the coroutine is not resumed and instead the
/// exception from the future is forwarded to the waiter directly and the
/// coroutine is destroyed.
///
/// For example:
/// ```
/// // Function careful to not throw exceptions, instead returning failed futures.
/// future<int> bar() {
///    if (something_bad_happened) {
///        return make_exception_future<>(std::runtime_error("error"));
///    }
///    return result;
/// }
///
/// future<> foo() {
///     auto result = co_await coroutine::try_future(bar());
///     // This code is only executed if bar() returned a successful future.
///     // Otherwise the exception is forwarded to the waiter future directly
///     // and the coroutine is destroyed.
///     check_result(result);
/// }
/// ```
///
/// Note that by default, `try_future` checks for if the task quota is depleted,
/// which means that it will yield if the future is ready and \ref seastar::need_preempt()
/// returns true.  Use \ref coroutine::try_future_without_preemption_check
/// to disable preemption checking.
template<typename T>
class [[nodiscard]] try_future : public seastar::internal::try_future_awaiter<true, T> {
public:
    explicit try_future(seastar::future<T>&& f) noexcept
        : seastar::internal::try_future_awaiter<true, T>(std::move(f))
    {}
};

/// \brief co_await:s a \ref future, returns the wrapped result if successful,
/// terminates the coroutine otherwise, propagating the exception to the waiter.
///
/// Same as \ref coroutine::try_future, but does not check for preemption.
template<typename T>
class [[nodiscard]] try_future_without_preemption_check : public seastar::internal::try_future_awaiter<false, T> {
public:
    explicit try_future_without_preemption_check(seastar::future<T>&& f) noexcept
        : seastar::internal::try_future_awaiter<false, T>(std::move(f))
    {}
};

} // namespace seastar::coroutine
