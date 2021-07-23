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

#include <seastar/core/std-coroutine.hh>
#include <seastar/coroutine/exception.hh>

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

        SEASTAR_INTERNAL_COROUTINE_NAMESPACE::suspend_never initial_suspend() noexcept { return { }; }
        SEASTAR_INTERNAL_COROUTINE_NAMESPACE::suspend_never final_suspend() noexcept { return { }; }

        virtual void run_and_dispose() noexcept override {
            auto handle = SEASTAR_INTERNAL_COROUTINE_NAMESPACE::coroutine_handle<promise_type>::from_promise(*this);
            handle.resume();
        }

        task* waiting_task() noexcept override { return _promise.waiting_task(); }
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

        void unhandled_exception() noexcept {
            _promise.set_exception(std::current_exception());
        }

        seastar::future<> get_return_object() noexcept {
            return _promise.get_future();
        }

        SEASTAR_INTERNAL_COROUTINE_NAMESPACE::suspend_never initial_suspend() noexcept { return { }; }
        SEASTAR_INTERNAL_COROUTINE_NAMESPACE::suspend_never final_suspend() noexcept { return { }; }

        virtual void run_and_dispose() noexcept override {
            auto handle = SEASTAR_INTERNAL_COROUTINE_NAMESPACE::coroutine_handle<promise_type>::from_promise(*this);
            handle.resume();
        }

       task* waiting_task() noexcept override { return _promise.waiting_task(); }
    };
};

template<typename... T>
struct awaiter {
    seastar::future<T...> _future;
public:
    explicit awaiter(seastar::future<T...>&& f) noexcept : _future(std::move(f)) { }

    awaiter(const awaiter&) = delete;
    awaiter(awaiter&&) = delete;

    bool await_ready() const noexcept {
        return _future.available() && !need_preempt();
    }

    template<typename U>
    void await_suspend(SEASTAR_INTERNAL_COROUTINE_NAMESPACE::coroutine_handle<U> hndl) noexcept {
        if (!_future.available()) {
            _future.set_coroutine(hndl.promise());
        } else {
            schedule(&hndl.promise());
        }
    }

    std::tuple<T...> await_resume() { return _future.get(); }
};

template<typename T>
struct awaiter<T> {
    seastar::future<T> _future;
public:
    explicit awaiter(seastar::future<T>&& f) noexcept : _future(std::move(f)) { }

    awaiter(const awaiter&) = delete;
    awaiter(awaiter&&) = delete;

    bool await_ready() const noexcept {
        return _future.available() && !need_preempt();
    }

    template<typename U>
    void await_suspend(SEASTAR_INTERNAL_COROUTINE_NAMESPACE::coroutine_handle<U> hndl) noexcept {
        if (!_future.available()) {
            _future.set_coroutine(hndl.promise());
        } else {
            schedule(&hndl.promise());
        }
    }

    T await_resume() { return _future.get0(); }
};

template<>
struct awaiter<> {
    seastar::future<> _future;
public:
    explicit awaiter(seastar::future<>&& f) noexcept : _future(std::move(f)) { }

    awaiter(const awaiter&) = delete;
    awaiter(awaiter&&) = delete;

    bool await_ready() const noexcept {
        return _future.available() && !need_preempt();
    }

    template<typename U>
    void await_suspend(SEASTAR_INTERNAL_COROUTINE_NAMESPACE::coroutine_handle<U> hndl) noexcept {
        if (!_future.available()) {
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
    return internal::awaiter<T...>(std::move(f));
}

} // seastar


namespace SEASTAR_INTERNAL_COROUTINE_NAMESPACE {

template<typename... T, typename... Args>
class coroutine_traits<seastar::future<T...>, Args...> : public seastar::internal::coroutine_traits_base<T...> {
};

} // SEASTAR_INTERNAL_COROUTINE_NAMESPACE

