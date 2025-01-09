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

#include <seastar/core/future.hh>
#include <coroutine>
#include <exception>

namespace seastar {

namespace internal {

struct exception_awaiter {
    std::exception_ptr eptr;

    explicit exception_awaiter(std::exception_ptr&& eptr) noexcept : eptr(std::move(eptr)) {}

    exception_awaiter(const exception_awaiter&) = delete;
    exception_awaiter(exception_awaiter&&) = delete;

    bool await_ready() const noexcept {
        return false;
    }

    template<typename U>
    void await_suspend(std::coroutine_handle<U> hndl) noexcept {
        hndl.promise().set_exception(std::move(eptr));
        hndl.destroy();
    }

    void await_resume() noexcept {}
};

} // internal

namespace coroutine {

/// Wrapper for propagating an exception directly rather than
/// throwing it. The wrapper can be used with both co_await and co_return.
///
/// \note It is not possible to co_return the wrapper in coroutines which
/// return future<> due to language limitations (it's not possible to specify
/// both return_value and return_void in the promise_type). You can use co_await
/// instead which works in coroutines which return either future<> or future<T>.
///
/// Example usage:
///
/// ```
/// co_await coroutine::exception(std::make_exception_ptr(std::runtime_error("something failed miserably")));
/// co_return coroutine::exception(std::make_exception_ptr(std::runtime_error("something failed miserably")));
/// ```
struct exception {
    std::exception_ptr eptr;
    explicit exception(std::exception_ptr eptr) noexcept : eptr(std::move(eptr)) {}
};

/// Allows propagating an exception from a coroutine directly rather than
/// throwing it.
///
/// `make_exception()` returns an object which must be co_returned.
/// Co_returning the object will immediately resolve the current coroutine
/// to the given exception.
///
/// \note Due to language limitations, this function doesn't work in coroutines
/// which return future<>. Consider using return_exception instead.
///
/// Example usage:
///
/// ```
/// co_return coroutine::make_exception(std::runtime_error("something failed miserably"));
/// ```
[[deprecated("Use co_await coroutine::return_exception_ptr or co_return coroutine::exception instead")]]
[[nodiscard]]
inline exception make_exception(std::exception_ptr ex) noexcept {
    return exception(std::move(ex));
}

template<typename T>
[[deprecated("Use co_await coroutine::return_exception_ptr or co_return coroutine::exception instead")]]
[[nodiscard]]
exception make_exception(T&& t) noexcept {
    log_exception_trace();
    return exception(std::make_exception_ptr(std::forward<T>(t)));
}

/// Allows propagating an exception from a coroutine directly rather than
/// throwing it.
///
/// `return_exception_ptr()` returns an object which must be co_awaited.
/// Co_awaiting the object will immediately resolve the current coroutine
/// to the given exception.
///
/// Example usage:
///
/// ```
/// std::exception_ptr ex;
/// try {
///   //
/// } catch (...) {
///   ex = std::current_exception();
/// }
/// if (ex) {
///   co_await coroutine::return_exception_ptr(std::move(ex));
/// }
/// ```
[[nodiscard]]
inline exception return_exception_ptr(std::exception_ptr ex) noexcept {
    return exception(std::move(ex));
}

/// Allows propagating an exception from a coroutine directly rather than
/// throwing it.
///
/// `return_exception()` returns an object which must be co_awaited.
/// Co_awaiting the object will immediately resolve the current coroutine
/// to the given exception.
///
/// Example usage:
///
/// ```
/// co_await coroutine::return_exception(std::runtime_error("something failed miserably"));
/// ```
[[deprecated("Use co_await coroutine::return_exception_ptr instead")]]
[[nodiscard]]
inline exception return_exception(std::exception_ptr ex) noexcept {
    return exception(std::move(ex));
}

template<typename T>
[[nodiscard]]
exception return_exception(T&& t) noexcept {
    log_exception_trace();
    return exception(std::make_exception_ptr(std::forward<T>(t)));
}

inline auto operator co_await(exception ex) noexcept {
    return internal::exception_awaiter(std::move(ex.eptr));
}

} // coroutine
} // seastar
