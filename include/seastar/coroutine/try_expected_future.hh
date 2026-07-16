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

#include <expected>
#include <utility>

#include <seastar/core/coroutine.hh>
#include <seastar/core/internal/current_task.hh>

namespace seastar::internal {

/// \brief Trait detecting whether \p T is a specialization of std::expected.
template <typename T>
struct is_std_expected : std::false_type {};

template <typename V, typename E>
struct is_std_expected<std::expected<V, E>> : std::true_type {};

template <typename T>
concept std_expected = is_std_expected<T>::value;

template <bool CheckPreempt, std_expected T>
class [[nodiscard]] try_expected_future_awaiter : public seastar::task {
    // The wrapped result type, e.g. std::expected<value_type, error_type>.
    using value_type = typename T::value_type;

    seastar::future<T> _future;
    void (*_resume_or_destroy)(try_expected_future_awaiter&){};
    seastar::task* _coroutine_task{};
    seastar::task* _waiting_task{};

    // Resumes the coroutine (expected holds a value) or completes and destroys it
    // (future failed, or expected holds an unexpected). On the value path the
    // expected is put back into _future so that await_resume() can hand out the
    // wrapped value.
    template <typename U>
    static void resume_or_destroy(try_expected_future_awaiter& self) {
        auto promise_ptr = static_cast<U*>(self._coroutine_task);
        auto hndl = std::coroutine_handle<U>::from_promise(*promise_ptr);

        if (self._future.failed()) {
            // Exceptional future: forward the exception to the waiter directly.
            hndl.promise().set_exception(std::move(self._future).get_exception());
            hndl.destroy();
            return;
        }

        T expected = std::move(self._future).get();
        if (!expected.has_value()) {
            // The expected holds an unexpected: rebind it to the coroutine's
            // return type and return with it, without resuming the coroutine.
            hndl.promise().return_value(std::unexpected(std::move(expected).error()));
            hndl.destroy();
            return;
        }

        // The expected holds a value: put it back and resume the coroutine,
        // await_resume() will hand out the wrapped value.
        self._future = seastar::make_ready_future<T>(std::move(expected));
        set_current_task(promise_ptr);
        hndl.resume();
    }

public:
    explicit try_expected_future_awaiter(seastar::future<T>&& f) noexcept : _future(std::move(f)) {}

    try_expected_future_awaiter(const try_expected_future_awaiter&) = delete;
    try_expected_future_awaiter(try_expected_future_awaiter&&) = delete;

    bool await_ready() noexcept {
        // A ready future has no non-destructive way to inspect its expected, so
        // unwrap it and put it right back. Only skip suspending when it holds a
        // value (the fast path); a failed future or an unexpected must suspend so
        // that the coroutine can be completed and destroyed without being resumed.
        if (!_future.available() || _future.failed() || (CheckPreempt && need_preempt())) {
            return false;
        }
        T expected = std::move(_future).get();
        bool has_value = expected.has_value();
        _future = seastar::make_ready_future<T>(std::move(expected));
        return has_value;
    }

    template<typename U>
    void await_suspend(std::coroutine_handle<U> hndl) noexcept {
        _resume_or_destroy = try_expected_future_awaiter::resume_or_destroy<U>;
        _coroutine_task = &hndl.promise();
        _waiting_task = hndl.promise().waiting_task();

        if (_future.available()) {
            execute_involving_handle_destruction_in_await_suspend(this);
        } else {
            _future.set_coroutine(*this);
        }
    }

    value_type await_resume() {
        // Only reached on the success path, where _future holds a value.
        return std::move(_future).get().value();
    }

    virtual void run_and_dispose() noexcept final override {
        _resume_or_destroy(*this);
    }

    virtual task* waiting_task() noexcept override {
        return _waiting_task;
    }
};

} // namespace seastar::internal

namespace seastar::coroutine {

/// \brief co_await:s a \ref future of a `std::expected` and returns the wrapped
/// value if successful, terminating the coroutine otherwise, propagating the
/// error directly to the waiter.
///
/// This is the `std::expected` counterpart of \ref coroutine::try_future. It
/// distinguishes three outcomes of the awaited `future<std::expected<V, E>>`:
///
///  - If the future failed (holds an exception), the coroutine is not resumed;
///    the exception is forwarded to the waiter directly and the coroutine is
///    destroyed, exactly like \ref coroutine::try_future.
///  - If the future resolved with an unexpected value, the coroutine is not
///    resumed; the `std::unexpected` is rebound to the coroutine's return type
///    (which must be a `future<std::expected<V2, E>>` with a matching error
///    type) and returned to the waiter, and the coroutine is destroyed.
///  - If the future resolved with a value, that value is the result of the
///    `co_await`.
///
/// This lets a coroutine propagate errors up the `std::expected` chain without
/// throwing, analogous to rust's
/// [try operator](https://google.github.io/comprehensive-rust/error-handling/try.html).
///
/// For example:
/// ```
/// future<std::expected<int, error>> bar();
///
/// future<std::expected<result, error>> foo() {
///     // If bar() resolves with an error (or a failed future), foo() returns
///     // with that error (or exception) directly and this coroutine stops here.
///     auto value = co_await coroutine::try_expected_future(bar());
///     // Only reached if bar() resolved with a value.
///     co_return compute_result(value);
/// }
/// ```
///
/// Note that by default, `try_expected_future` checks if the task quota is
/// depleted, which means that it will yield if the future is ready and
/// \ref seastar::need_preempt() returns true. Use
/// \ref coroutine::try_expected_future_without_preemption_check to disable
/// preemption checking.
///
/// \tparam T the `std::expected` specialization wrapped by the future.
template<seastar::internal::std_expected T>
class [[nodiscard]] try_expected_future : public seastar::internal::try_expected_future_awaiter<true, T> {
public:
    explicit try_expected_future(seastar::future<T>&& f) noexcept
        : seastar::internal::try_expected_future_awaiter<true, T>(std::move(f))
    {}
};

/// \brief co_await:s a \ref future of a `std::expected`, returns the wrapped
/// value if successful, terminating the coroutine otherwise, propagating the
/// error to the waiter.
///
/// Same as \ref coroutine::try_expected_future, but does not check for
/// preemption.
template<seastar::internal::std_expected T>
class [[nodiscard]] try_expected_future_without_preemption_check : public seastar::internal::try_expected_future_awaiter<false, T> {
public:
    explicit try_expected_future_without_preemption_check(seastar::future<T>&& f) noexcept
        : seastar::internal::try_expected_future_awaiter<false, T>(std::move(f))
    {}
};

} // namespace seastar::coroutine
