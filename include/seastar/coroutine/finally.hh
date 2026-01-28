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

#include <seastar/core/coroutine.hh>
#include <seastar/core/internal/current_task.hh>

namespace seastar::internal {

template <bool CheckPreempt, typename Func>
class [[nodiscard]] finally_awaiter {
    Func _func;

public:
    explicit finally_awaiter(Func&& func) noexcept : _func(std::forward<Func>(func)) {}

    finally_awaiter(const finally_awaiter&) = delete;
    finally_awaiter(finally_awaiter&&) = delete;

    bool await_ready() const noexcept { return false; }

    template <typename U>
    void await_suspend(std::coroutine_handle<U> hndl) noexcept {
        using T = typename U::value_type;

        seastar::promise<T> promise;

        promise.get_future().then_wrapped([func = std::move(_func)] (future<T> f) mutable noexcept -> future<T> {
            std::exception_ptr ex;

            if (f.failed()) {
                ex = f.get_exception();
                f = seastar::make_exception_future<T>(std::exception_ptr(ex));
            }

            auto intermediate_f = make_ready_future<>();
            if constexpr (std::invocable<Func, std::exception_ptr>) {
                intermediate_f = futurize_invoke(func, std::move(ex));
            } else {
                intermediate_f = futurize_invoke(func);
            }

            return intermediate_f.then_wrapped([f = std::move(f)] (future<> finally_f) mutable noexcept -> future<T> {
                if (finally_f.failed()) {
                    if (f.failed()) {
                        // Both the main future and the finally function failed.
                        // We wrap both exceptions in a nested_exception to
                        // not lose either of them.
                        auto nested_ex = std::make_exception_ptr(nested_exception(f.get_exception(), finally_f.get_exception()));
                        return seastar::make_exception_future<T>(std::move(nested_ex));
                    }
                    return seastar::make_exception_future<T>(finally_f.get_exception());
                }
                return std::move(f);
            });
        }).forward_to(hndl.promise().exchange_underlying_promise(std::move(promise)));

        if (CheckPreempt && need_preempt()) {
            schedule(&hndl.promise());
        } else {
            hndl.resume();
        }
    }

    void await_resume() { }
};

} // namespace seastar::internal

namespace seastar::coroutine {

template<typename Func>
concept FinallyFunc =
    requires(Func f, std::exception_ptr ex) {
        { f(ex) } -> std::same_as<future<>>; 
    } || requires(Func f) {
        { f() } -> std::same_as<future<>>; 
    };

/// \brief schedules the function to be run right after the coroutine finished.
///
/// Useful to clean-up resources acquired in a coroutine. The function can be
/// asycronous.
///
/// The lambda passed to `finally` can optionally take a single parameter of type
/// `std::exception_ptr`, which will contain the exception the coroutine resolved
/// with, if it resolved with an exception. Useful to do logging or different
/// clean-up based on success/failure. The exception cannot be handled -- it will
/// be propagated to the coroutine's caller, regardless of what is done in the
/// `finally` function.
///
/// Similar to `future<>::finally()`, the function is called after the coroutine
/// is finished, regardless of whether it completed with success or failure.
///
/// Also similar to `future<>::finally()`, the lambda should not capture local
/// variables via references, as it is executed after the coroutine frame was
/// destroyed.
///
/// Example:
/// ```
/// future<> my_coroutine() {
///     auto file = co_await open_file_dma("myfile");
///
///     co_await coroutine::finally([file] (std::exception_ptr eptr) {
///        return file.close();
///     });
///
///     // do things with file (which might throw)
/// }
/// ```
///
/// Note that by default, `finally` checks for if the task quota is depleted,
/// which means that it will yield if \ref seastar::need_preempt()
/// returns true. Use \ref coroutine::finally_without_preemption_check
/// to disable preemption checking.
template <FinallyFunc Func>
class [[nodiscard]] finally : public seastar::internal::finally_awaiter<true, Func> {
public:
    explicit finally(Func&& func) noexcept
        : seastar::internal::finally_awaiter<true, Func>(std::forward<Func>(func))
    {}
};

/// \brief schedules the function to be run right after the coroutine finished.
///
/// Same as \ref coroutine::finally, but does not check for preemption.
template <FinallyFunc Func>
class [[nodiscard]] finally_without_preemption_check : public seastar::internal::finally_awaiter<false, Func> {
public:
    explicit finally_without_preemption_check(Func&& func) noexcept
        : seastar::internal::finally_awaiter<false, Func>(std::forward<Func>(func))
    {}
};

} // namespace seastar::coroutine
