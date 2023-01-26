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
 * Copyright (C) 2019 ScyllaDB Ltd.
 */

#pragma once

#include <seastar/core/future.hh>

#ifndef SEASTAR_COROUTINES_ENABLED
#error Coroutines support disabled.
#endif

#include <seastar/coroutine/exception.hh>
#include <coroutine>

namespace seastar {

namespace internal {

template <typename T = void>
class coroutine_traits_base {
public:
    class promise_type final : public seastar::task {
        seastar::promise<T> _promise;
    public:
        promise_type() = default;
        promise_type(promise_type&&) = delete;
        promise_type(const promise_type&) = delete;

        template<typename... U>
        void return_value(U&&... value) {
            _promise.set_value(std::forward<U>(value)...);
        }

        void return_value(coroutine::exception ce) noexcept {
            _promise.set_exception(std::move(ce.eptr));
        }

        void set_exception(std::exception_ptr&& eptr) noexcept {
            _promise.set_exception(std::move(eptr));
        }

        [[deprecated("Forwarding coroutine returns are deprecated as too dangerous. Use 'co_return co_await ...' until explicit syntax is available.")]]
        void return_value(future<T>&& fut) noexcept {
            fut.forward_to(std::move(_promise));
        }

        void unhandled_exception() noexcept {
            _promise.set_exception(std::current_exception());
        }

        seastar::future<T> get_return_object() noexcept {
            return _promise.get_future();
        }

        std::suspend_never initial_suspend() noexcept { return { }; }
        std::suspend_never final_suspend() noexcept { return { }; }

        virtual void run_and_dispose() noexcept override {
            auto handle = std::coroutine_handle<promise_type>::from_promise(*this);
            handle.resume();
        }

        task* waiting_task() noexcept override { return _promise.waiting_task(); }

        scheduling_group set_scheduling_group(scheduling_group sg) noexcept {
            return std::exchange(this->_sg, sg);
        }
    };
};

template <>
class coroutine_traits_base<> {
public:
   class promise_type final : public seastar::task {
        seastar::promise<> _promise;
    public:
        promise_type() = default;
        promise_type(promise_type&&) = delete;
        promise_type(const promise_type&) = delete;

        void return_void() noexcept {
            _promise.set_value();
        }

        void set_exception(std::exception_ptr&& eptr) noexcept {
            _promise.set_exception(std::move(eptr));
        }

        void unhandled_exception() noexcept {
            _promise.set_exception(std::current_exception());
        }

        seastar::future<> get_return_object() noexcept {
            return _promise.get_future();
        }

        std::suspend_never initial_suspend() noexcept { return { }; }
        std::suspend_never final_suspend() noexcept { return { }; }

        virtual void run_and_dispose() noexcept override {
            auto handle = std::coroutine_handle<promise_type>::from_promise(*this);
            handle.resume();
        }

        task* waiting_task() noexcept override { return _promise.waiting_task(); }

        scheduling_group set_scheduling_group(scheduling_group new_sg) noexcept {
            return task::set_scheduling_group(new_sg);
        }
    };
};

template<bool CheckPreempt, typename... T>
struct awaiter {
    seastar::future<T...> _future;
public:
    explicit awaiter(seastar::future<T...>&& f) noexcept : _future(std::move(f)) { }

    awaiter(const awaiter&) = delete;
    awaiter(awaiter&&) = delete;

    bool await_ready() const noexcept {
        return _future.available() && (!CheckPreempt || !need_preempt());
    }

    template<typename U>
    void await_suspend(std::coroutine_handle<U> hndl) noexcept {
        if (!CheckPreempt || !_future.available()) {
            _future.set_coroutine(hndl.promise());
        } else {
            schedule(&hndl.promise());
        }
    }

    std::tuple<T...> await_resume() { return _future.get(); }
};

template<bool CheckPreempt, typename T>
struct awaiter<CheckPreempt, T> {
    seastar::future<T> _future;
public:
    explicit awaiter(seastar::future<T>&& f) noexcept : _future(std::move(f)) { }

    awaiter(const awaiter&) = delete;
    awaiter(awaiter&&) = delete;

    bool await_ready() const noexcept {
        return _future.available() && (!CheckPreempt || !need_preempt());
    }

    template<typename U>
    void await_suspend(std::coroutine_handle<U> hndl) noexcept {
        if (!CheckPreempt || !_future.available()) {
            _future.set_coroutine(hndl.promise());
        } else {
            schedule(&hndl.promise());
        }
    }

    T await_resume() { return _future.get0(); }
};

template<bool CheckPreempt>
struct awaiter<CheckPreempt> {
    seastar::future<> _future;
public:
    explicit awaiter(seastar::future<>&& f) noexcept : _future(std::move(f)) { }

    awaiter(const awaiter&) = delete;
    awaiter(awaiter&&) = delete;

    bool await_ready() const noexcept {
        return _future.available() && (!CheckPreempt || !need_preempt());
    }

    template<typename U>
    void await_suspend(std::coroutine_handle<U> hndl) noexcept {
        if (!CheckPreempt || !_future.available()) {
            _future.set_coroutine(hndl.promise());
        } else {
            schedule(&hndl.promise());
        }
    }

    void await_resume() { _future.get(); }
};

} // seastar::internal

template<typename... T>
auto operator co_await(future<T...> f) noexcept {
    return internal::awaiter<true, T...>(std::move(f));
}

namespace coroutine {
/// Wrapper for a future which turns off checking for preemption
/// when awaiting it in a coroutine.
/// If constructed from a future, co_await-ing it will bypass
/// checking if the task quota is depleted, which means that
/// a ready future will be handled immediately.
template<typename... T> struct SEASTAR_NODISCARD without_preemption_check : public seastar::future<T...> {
    explicit without_preemption_check(seastar::future<T...>&& f) noexcept : seastar::future<T...>(std::move(f)) {}
};
template<typename T> struct SEASTAR_NODISCARD without_preemption_check<T> : public seastar::future<T> {
    explicit without_preemption_check(seastar::future<T>&& f) noexcept : seastar::future<T>(std::move(f)) {}
};
template<> struct SEASTAR_NODISCARD without_preemption_check<> : public seastar::future<> {
    explicit without_preemption_check(seastar::future<>&& f) noexcept : seastar::future<>(std::move(f)) {}
};

/// Make a lambda coroutine safe for use in an outer coroutine with
/// functions that accept continuations.
///
/// A lambda coroutine is not a safe parameter to a function that expects
/// a regular Seastar continuation.
///
/// To use, wrap the lambda coroutine in seastar::coroutine::lambda(). The
/// lambda coroutine must complete (co_await) in the same statement.
///
/// Example::
/// ```
///    // `future::then()` expects a continuation, so not safe for lambda
///    // coroutines without seastar::coroutine::lambda.
///    co_await seastar::yield().then(seastar::coroutine::lambda([captures] () -> future<> {
///        co_await seastar::coroutine::maybe_yield();
///        // use of `captures` here can break without seastar::coroutine::lambda.
///    }));
/// ```
///
/// \tparam Func type of function object (typically inferred)
template <typename Func>
class lambda {
    Func* _func;
public:
    /// Create a lambda coroutine wrapper from a function object, to be passed
    /// to a Seastar function that accepts a continuation.
    explicit lambda(Func&& func) : _func(&func) {}
    /// Calls the lambda coroutine object. Normally invoked by Seastar.
    template <typename... Args>
    decltype(auto) operator()(Args&&... args) const {
        return std::invoke(*_func, std::forward<Args>(args)...);
    }
};

}

/// Wait for a future without a preemption check
///
/// \param f a \c future<> wrapped with \c without_preemption_check
template<typename... T>
auto operator co_await(coroutine::without_preemption_check<T...> f) noexcept {
    return internal::awaiter<false, T...>(std::move(f));
}

} // seastar


namespace std {

template<typename... T, typename... Args>
class coroutine_traits<seastar::future<T...>, Args...> : public seastar::internal::coroutine_traits_base<T...> {
};

} // std

