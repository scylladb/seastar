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

// Whether try_future should propagate an unexpected value of a
// future<std::expected<...>> result by exiting the coroutine (rebinding it to
// the coroutine's return type), rather than handing it out as a plain value.
// Controlled by the SEASTAR_TRY_FUTURE_EXPECTED opt-in, on by default.
#ifdef SEASTAR_TRY_FUTURE_EXPECTED
template <typename T>
inline constexpr bool try_future_propagates_expected = std_expected<T>;
#else
template <typename T>
inline constexpr bool try_future_propagates_expected = false;
#endif

template <typename T, typename U>
void try_future_resume_or_destroy_coroutine(seastar::future<T>& fut, seastar::task& coroutine_task) {
    auto promise_ptr = static_cast<U*>(&coroutine_task);
    auto hndl = std::coroutine_handle<U>::from_promise(*promise_ptr);

    if (fut.failed()) {
        // Exceptional future: forward the exception to the waiter directly.
        hndl.promise().set_exception(std::move(fut).get_exception());
        hndl.destroy();
        return;
    }

    if constexpr (try_future_propagates_expected<T>) {
        T expected = std::move(fut).get();
        if (!expected.has_value()) {
            // The expected holds an unexpected: rebind it to the coroutine's
            // return type and return with it, without resuming the coroutine.
            hndl.promise().return_value(std::unexpected(std::move(expected).error()));
            hndl.destroy();
            return;
        }
        // The expected holds a value: put it back so that await_resume() can
        // hand out the wrapped value, then resume the coroutine.
        fut = seastar::make_ready_future<T>(std::move(expected));
    }

    set_current_task(promise_ptr);
    hndl.resume();
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

    bool await_ready() noexcept {
        // Will suspend+schedule for ready failed futures too.
        if (!_future.available() || _future.failed() || (CheckPreempt && need_preempt())) {
            return false;
        }
        if constexpr (try_future_propagates_expected<T>) {
            // A ready future has no non-destructive way to inspect its expected,
            // so unwrap it and put it right back. Only take the fast path when it
            // holds a value; an unexpected must suspend so that the coroutine can
            // be completed and destroyed without being resumed.
            T expected = std::move(_future).get();
            bool has_value = expected.has_value();
            _future = seastar::make_ready_future<T>(std::move(expected));
            return has_value;
        } else {
            return true;
        }
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

    // Returns the unwrapped value_type on the std::expected fast path (only
    // reached when the expected holds a value), otherwise the whole result T.
    auto await_resume() {
        if constexpr (try_future_propagates_expected<T>) {
            return std::move(_future).get().value();
        } else if constexpr (std::is_void_v<T>) {
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
///
/// ## `std::expected` results
///
/// When the awaited future wraps a `std::expected<V, E>`, `try_future` extends
/// its error propagation to unexpected values, treating them like a rust-style
/// [try operator](https://google.github.io/comprehensive-rust/error-handling/try.html)
/// on top of the exception handling above. It distinguishes three outcomes:
///
///  - If the future failed (holds an exception), the coroutine is not resumed;
///    the exception is forwarded to the waiter directly and the coroutine is
///    destroyed, exactly as for a non-`std::expected` result.
///  - If the future resolved with an unexpected value, the coroutine is not
///    resumed; the `std::unexpected` is rebound to the coroutine's return type
///    (which must be a `future<std::expected<V2, E>>` with a matching error
///    type) and returned to the waiter, and the coroutine is destroyed.
///  - If the future resolved with a value, that value is the result of the
///    `co_await`.
///
/// For example:
/// ```
/// future<std::expected<int, error>> bar();
///
/// future<std::expected<result, error>> foo() {
///     // If bar() resolves with an error (or a failed future), foo() returns
///     // with that error (or exception) directly and this coroutine stops here.
///     auto value = co_await coroutine::try_future(bar());
///     // Only reached if bar() resolved with a value.
///     co_return compute_result(value);
/// }
/// ```
///
/// This `std::expected`-aware behavior is enabled by the
/// `SEASTAR_TRY_FUTURE_EXPECTED` macro, which is defined by default. When it is
/// not defined, awaiting a `future<std::expected<V, E>>` is deprecated and
/// behaves like any other result: only a failed future exits the coroutine, and
/// an unexpected value is handed out as the (whole `std::expected`) result of
/// the `co_await`.
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

#ifndef SEASTAR_TRY_FUTURE_EXPECTED

/// \brief Deprecated legacy specialization for `future<std::expected>`.
///
/// When `SEASTAR_TRY_FUTURE_EXPECTED` is not defined, awaiting a
/// `future<std::expected<V, E>>` does not treat an unexpected value specially: it
/// behaves like the generic \ref try_future, handing the whole `std::expected`
/// out as the result of the `co_await`. Define `SEASTAR_TRY_FUTURE_EXPECTED`
/// (the default) to have unexpected values exit the coroutine instead.
template<seastar::internal::std_expected T>
class [[nodiscard]] try_future<T> : public seastar::internal::try_future_awaiter<true, T> {
public:
    [[deprecated("try_future on a future<std::expected> will propagate unexpected values; "
                 "define SEASTAR_TRY_FUTURE_EXPECTED to opt in to the new behavior")]]
    explicit try_future(seastar::future<T>&& f) noexcept
        : seastar::internal::try_future_awaiter<true, T>(std::move(f))
    {}
};

/// \brief Deprecated legacy specialization for `future<std::expected>`, without
/// preemption checking. See \ref try_future.
template<seastar::internal::std_expected T>
class [[nodiscard]] try_future_without_preemption_check<T> : public seastar::internal::try_future_awaiter<false, T> {
public:
    [[deprecated("try_future_without_preemption_check on a future<std::expected> will propagate "
                 "unexpected values; define SEASTAR_TRY_FUTURE_EXPECTED to opt in to the new behavior")]]
    explicit try_future_without_preemption_check(seastar::future<T>&& f) noexcept
        : seastar::internal::try_future_awaiter<false, T>(std::move(f))
    {}
};

#endif // SEASTAR_TRY_FUTURE_EXPECTED

} // namespace seastar::coroutine
