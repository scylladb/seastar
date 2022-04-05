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

#include <seastar/core/coroutine.hh>

namespace seastar {

namespace internal {

template <bool CheckPreempt, typename T>
class SEASTAR_NODISCARD as_future_awaiter {
    seastar::future<T> _future;

public:
    explicit as_future_awaiter(seastar::future<T>&& f) noexcept : _future(std::move(f)) {}

    as_future_awaiter(const as_future_awaiter&) = delete;
    as_future_awaiter(as_future_awaiter&&) = delete;

    bool await_ready() const noexcept {
        return _future.available() && (!CheckPreempt || !need_preempt());
    }

    template<typename U>
    void await_suspend(SEASTAR_INTERNAL_COROUTINE_NAMESPACE::coroutine_handle<U> hndl) noexcept {
        if (!CheckPreempt || !_future.available()) {
            _future.set_coroutine(hndl.promise());
        } else {
            schedule(&hndl.promise());
        }
    }

    seastar::future<T> await_resume() {
        return std::move(_future);
    }
};

} // namespace seastar::internal

namespace coroutine {

/// \brief co_await:s a \ref future, returning it as result.
///
/// Similar to \ref seastar::future::then_wrapped, `coroutine::as_future`
/// waits for the \ref future to resolve either to a ready future
/// or to an exceptional one. It then returns it as the co_await result.
///
/// For example:
/// ```
/// static future<bool> did_future_fail(future<> fut) {
///     auto f = co_await coroutine::as_future(std::move(fut));
///     if (f.failed()) {
///         mylog.warn("Future failed: {}", f.get_exception());
///         co_return true;
///     } else {
///         co_return false;
///     }
/// }
/// ```
///
/// Note that by default, `as_future` checks for if the task quota is depleted,
/// which means that it will yield if the future is ready and \ref seastar::need_preempt()
/// returns true.  Use \ref coroutine::as_future_without_preemption_check
/// to disable preemption checking.
template<typename T = void>
class SEASTAR_NODISCARD as_future : public seastar::internal::as_future_awaiter<true, T> {
public:
    explicit as_future(seastar::future<T>&& f) noexcept : seastar::internal::as_future_awaiter<true, T>(std::move(f)) {}
};

/// \brief co_await:s a \ref future, returning it as result, without
/// checking if preemption is needed.
///
/// Like \ref coroutine::as_future, co_await-ing as_future_without_preemption_check
/// returns the input `future` as the co_await result.
/// However, it bypasses checking if the task quota is depleted, which means that
/// a ready `future` will be handled immediately.
template<typename T = void>
class SEASTAR_NODISCARD as_future_without_preemption_check : public seastar::internal::as_future_awaiter<false, T> {
public:
    explicit as_future_without_preemption_check(seastar::future<T>&& f) noexcept : seastar::internal::as_future_awaiter<false, T>(std::move(f)) {}
};

} // namespace seastar::coroutine

} // namespace seastar
