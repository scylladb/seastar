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
 * Copyright (C) 2026-present ScyllaDB
 */

#pragma once

#include <seastar/util/noncopyable_function.hh>
#include <seastar/core/coroutine.hh>

namespace seastar::internal {

template <std::invocable<> Func>
struct at_coroutine_exit_wrapper {
    Func func;

    future<> operator()(std::exception_ptr) {
        return func();
    }
};

}

namespace seastar::coroutine {

/// \brief schedules the function to be run at coroutine exit
///
/// After the coroutine body has finished, but before the coroutine frame is
/// destroyed. Useful to clean-up resources acquired in a coroutine.
///
/// Equivalent to injecting `co_await func()` to the end of the coroutine, to run
/// regardless of the return path taken and regardless of whether the coroutine
/// resolved with a value or an exception.
///
/// When the function is called, the coroutine frame is still alive, so capturing
/// local variables *is* allowed.
///
/// Multiple calls to `schedule_at_coroutine_exit()` are allowed, the functions
/// will be executed in reverse (LIFO) order.
///
/// Exceptions from `func()` will be propagated to the coroutine's caller.
/// If both the coroutine and one or more coroutine exit func throws, the
/// final exception will be a seastar::nested_exception containing all of them,
/// with the inner-most exception being the one from the coroutine.
///
/// The lambda passed to `schedule_at_coroutine_exit()` can optionally take a single
/// parameter of type `std::exception_ptr`, which will contain the exception the
/// coroutine resolved with, if it resolved with an exception. Useful to do
/// logging or different clean-up based on success/failure. The exception cannot
/// be handled -- it will be propagated to the coroutine's caller, regardless of
/// what is done in the `schedule_at_coroutine_exit()` function.
///
/// Example:
/// ```
/// future<> my_coroutine() {
///     auto file = co_await open_file_dma("myfile");
///
///     co_await coroutine::schedule_at_coroutine_exit([&file] (std::exception_ptr eptr) {
///        return file.close();
///     });
///
///     // do things with file (which might throw)
/// }
/// ```
class schedule_at_coroutine_exit {
    noncopyable_function<future<>(std::exception_ptr)> _func;

public:
    explicit schedule_at_coroutine_exit(noncopyable_function<future<>(std::exception_ptr)> func) : _func(std::move(func)) {}

    template <std::invocable<> Func>
    explicit schedule_at_coroutine_exit(Func func) : _func(seastar::internal::at_coroutine_exit_wrapper{std::move(func)}) { }

    bool await_ready() const noexcept { return false; }

    template<typename U>
    void await_suspend(std::coroutine_handle<U> hndl) {
        hndl.promise().push_at_exit_function(std::move(_func));
        hndl.resume();
    }

    void await_resume() const noexcept {}
};

} // namespace seastar::coroutine
