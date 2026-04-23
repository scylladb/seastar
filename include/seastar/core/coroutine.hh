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
#include <seastar/core/make_task.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/util/std-compat.hh>


#include <coroutine>
#include <new>
#include <cstdlib>
#include <source_location>

#if !(__GLIBC__ == 2 && __GLIBC_MINOR__ >= 43) && !(__GLIBC__ > 2)

extern "C" {

void free_sized(void* ptr, size_t size);

}

#endif

namespace seastar {

namespace internal {


inline
void
execute_involving_handle_destruction_in_await_suspend(std::invocable<> auto&& func) noexcept {
#if defined(__GNUC__) && !defined(__clang__) && (__GNUC__ > 15 || (__GNUC__ == 15 && __GNUC_MINOR__ >= 2))
    memory::scoped_critical_alloc_section _;
    schedule(new lambda_task(current_scheduling_group(), std::forward<decltype(func)>(func)));
#else
    std::invoke(std::forward<decltype(func)>(func));
#endif
}

inline
void
execute_involving_handle_destruction_in_await_suspend(task* tsk) noexcept {
#if defined(__GNUC__) && !defined(__clang__) && (__GNUC__ > 15 || (__GNUC__ == 15 && __GNUC_MINOR__ >= 2))
    schedule(tsk);
#else
    tsk->run_and_dispose();
#endif
}

class coroutine_allocators {
public:
    static void* operator new(size_t size) {
        memory::scoped_critical_alloc_section _;
        return ::malloc(size);
    }
    static void operator delete(void* ptr) noexcept {
        ::free(ptr);
    }
#ifdef __cpp_sized_deallocation
    static void operator delete(void* ptr, std::size_t sz) noexcept {
        ::free_sized(ptr, sz);
    }
#endif
};

using at_exit_function = noncopyable_function<future<>(std::exception_ptr)>;

class at_exit_func_stack {
    std::optional<at_exit_function> _func;
    std::unique_ptr<at_exit_func_stack> _next;

public:
    at_exit_func_stack() = default;
    explicit at_exit_func_stack(at_exit_function&& func) : _func(std::move(func)) { }

    bool empty() const noexcept { return !_func; }

    at_exit_function& top() noexcept { return *_func; }

    void push(at_exit_function&& func) {
        auto new_node = _func ? std::make_unique<at_exit_func_stack>(std::move(*this)) : nullptr;
        _func = std::move(func);
        _next = std::move(new_node);
    }

    void pop() noexcept {
        _func.reset();
        if (auto next = std::exchange(_next, nullptr)) {
            new (this) at_exit_func_stack(std::move(*next));
        }
    }
};

template <typename U>
void destroy_coroutine(seastar::task& coroutine_task) {
    auto promise_ptr = static_cast<U*>(&coroutine_task);
    auto hndl = std::coroutine_handle<U>::from_promise(*promise_ptr);
    hndl.destroy();
}

inline std::exception_ptr combine_exceptions(std::exception_ptr ex1, std::exception_ptr ex2) noexcept {
    if (ex1 && ex2) {
        return std::make_exception_ptr(nested_exception(std::move(ex1), std::move(ex2)));
    } else if (ex1) {
        return ex1;
    } else {
        return ex2;
    }
}

class at_coroutine_exit_base : public seastar::task {
protected:
    at_exit_func_stack _funcs;

private:
    seastar::task* _coroutine_task;
    seastar::task* _waiting_task;
    void (*_destroy_coroutine)(seastar::task&);

    std::optional<seastar::future<>> _future;
    std::exception_ptr _ex;

private:
    virtual std::exception_ptr get_exception() noexcept = 0;
    virtual void resume_after_at_exit(std::exception_ptr at_exit_ex) noexcept = 0;

    future<> execute_one() noexcept {
        try {
            return _funcs.top()(get_exception());
        } catch (...) {
            return seastar::make_exception_future<>(std::current_exception());
        }
    }

public:
    at_coroutine_exit_base() = default;
    at_coroutine_exit_base(at_coroutine_exit_base&&) = delete;
    at_coroutine_exit_base(const at_coroutine_exit_base&) = delete;

    template <typename U>
    void run(std::coroutine_handle<U> hndl) noexcept {
        _coroutine_task = &hndl.promise();
        _waiting_task = hndl.promise().waiting_task();
        _destroy_coroutine = destroy_coroutine<U>;
        _future = execute_one();

        if (_future->available()) {
            execute_involving_handle_destruction_in_await_suspend(this);
        } else {
            _future->set_coroutine(*this);
        }
    }

    virtual void run_and_dispose() noexcept final override {
        while (_future->available()) {
            if (_future->failed()) {
                _ex = combine_exceptions(std::move(_ex), _future->get_exception());
            }

            _funcs.pop();

            if (!_funcs.empty()) {
                _future = execute_one();
                continue;
            }

            resume_after_at_exit(std::move(_ex));
            _destroy_coroutine(*_coroutine_task);

            return;
        }

        _future->set_coroutine(*this);
    }

    virtual task* waiting_task() noexcept final override {
        return _waiting_task;
    }
};

template <typename T = void>
class at_coroutine_exit final : public at_coroutine_exit_base {
    seastar::promise<T>& _proxy_promise;
    seastar::promise<T> _return_promise;

private:
    virtual std::exception_ptr get_exception() noexcept final override {
        return _proxy_promise.get_exception();
    }

    virtual void resume_after_at_exit(std::exception_ptr at_exit_ex) noexcept final override {
        auto proxy_fut = _proxy_promise.get_future();
        if (at_exit_ex) {
            auto final_ex = combine_exceptions(std::move(at_exit_ex), _proxy_promise.get_exception());
            proxy_fut.ignore_ready_future();
            proxy_fut = seastar::make_exception_future<T>(std::move(final_ex));
        }
        proxy_fut.forward_to(std::move(_return_promise));
    }

public:
    explicit at_coroutine_exit(seastar::promise<T>& promise)
        : _proxy_promise(promise)
        , _return_promise(std::exchange(_proxy_promise, seastar::promise<T>{}))
    { }

    seastar::promise<T>& get_return_promise() { return _return_promise; }

    void push_at_exit_function(at_exit_function&& func) {
        _funcs.push(std::move(func));
    }
};

class final_awaiter final {
    at_coroutine_exit_base* _at_exit;

public:
    explicit final_awaiter(at_coroutine_exit_base* at_exit) noexcept : _at_exit(at_exit) { }

    bool await_ready() const noexcept { return !_at_exit; }

    void await_resume() noexcept { }

    template <typename U>
    void await_suspend(std::coroutine_handle<U> hndl) noexcept { _at_exit->run(hndl); }
};

// Does not implement the full awaiter control semantics described at
// https://en.cppreference.com/w/cpp/language/coroutines.html.
// It is enough if this is compatible with our promise_type<>::final_suspend().
template <typename U>
void finalize_and_destroy_coroutine(std::coroutine_handle<U> hndl) {
    auto final_suspend = hndl.promise().final_suspend();
    if (final_suspend.await_ready()) {
        final_suspend.await_resume();
        hndl.destroy();
    } else {
        final_suspend.await_suspend(hndl);
    }
}

template <typename T = void>
class coroutine_traits_base {
public:
    class promise_type final : public seastar::task, public coroutine_allocators {
        seastar::promise<T> _promise;
        seastar::promise<T>* _return_promise;
        std::unique_ptr<at_coroutine_exit<T>> _at_exit;

    public:
        promise_type() : _return_promise(&_promise) { }
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
            return _return_promise->get_future();
        }

        std::suspend_never initial_suspend() noexcept { return { }; }
        final_awaiter final_suspend() noexcept { return final_awaiter(_at_exit.get()); }

        virtual void run_and_dispose() noexcept override {
            auto handle = std::coroutine_handle<promise_type>::from_promise(*this);
            handle.resume();
        }

        task* waiting_task() noexcept override { return _return_promise->waiting_task(); }

        scheduling_group set_scheduling_group(scheduling_group sg) noexcept {
            return std::exchange(this->_sg, sg);
        }

        void push_at_exit_function(at_exit_function&& func) {
            if (!_at_exit) {
                _at_exit = std::make_unique<at_coroutine_exit<T>>(_promise);
                _return_promise = &_at_exit->get_return_promise();
            }
            _at_exit->push_at_exit_function(std::move(func));
        }
    };
};

template <>
class coroutine_traits_base<> {
public:
   class promise_type final : public seastar::task, public coroutine_allocators {
        seastar::promise<> _promise;
        seastar::promise<>* _return_promise;
        std::unique_ptr<at_coroutine_exit<>> _at_exit;

    public:
        promise_type() : _return_promise(&_promise) { }
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
            return _return_promise->get_future();
        }

        std::suspend_never initial_suspend() noexcept { return { }; }
        final_awaiter final_suspend() noexcept { return final_awaiter(_at_exit.get()); }

        virtual void run_and_dispose() noexcept override {
            auto handle = std::coroutine_handle<promise_type>::from_promise(*this);
            handle.resume();
        }

        task* waiting_task() noexcept override { return _return_promise->waiting_task(); }

        scheduling_group set_scheduling_group(scheduling_group new_sg) noexcept {
            return task::set_scheduling_group(new_sg);
        }

        void push_at_exit_function(at_exit_function&& func) {
            if (!_at_exit) {
                _at_exit = std::make_unique<at_coroutine_exit<>>(_promise);
                _return_promise = &_at_exit->get_return_promise();
            }
            _at_exit->push_at_exit_function(std::move(func));
        }
    };
};

template<bool CheckPreempt, typename T>
struct awaiter {
    seastar::future<T> _future;
public:
    explicit awaiter(seastar::future<T>&& f) noexcept : _future(std::move(f)) { }

    awaiter(const awaiter&) = delete;
    awaiter(awaiter&&) = delete;

    bool await_ready() const noexcept {
        return _future.available() && (!CheckPreempt || !need_preempt());
    }

    template<typename U>
    void await_suspend(std::coroutine_handle<U> hndl SEASTAR_COROUTINE_LOC_PARAM) noexcept {
        SEASTAR_COROUTINE_LOC_STORE(hndl.promise());
        if (!CheckPreempt || !_future.available()) {
            _future.set_coroutine(hndl.promise());
        } else {
            schedule(&hndl.promise());
        }
    }

    T await_resume() { return _future.get(); }
};

template<bool CheckPreempt>
struct awaiter<CheckPreempt, void> {
    seastar::future<> _future;
public:
    explicit awaiter(seastar::future<>&& f) noexcept : _future(std::move(f)) { }

    awaiter(const awaiter&) = delete;
    awaiter(awaiter&&) = delete;

    bool await_ready() const noexcept {
        return _future.available() && (!CheckPreempt || !need_preempt());
    }

    template<typename U>
    void await_suspend(std::coroutine_handle<U> hndl SEASTAR_COROUTINE_LOC_PARAM) noexcept {
        SEASTAR_COROUTINE_LOC_STORE(hndl.promise());
        if (!CheckPreempt || !_future.available()) {
            _future.set_coroutine(hndl.promise());
        } else {
            schedule(&hndl.promise());
        }
    }

    void await_resume() { _future.get(); }
};

} // seastar::internal


template<typename T>
auto operator co_await(future<T>&& f) noexcept {
    return internal::awaiter<true, T>(std::move(f));
}

namespace coroutine {
/// Wrapper for a future which turns off checking for preemption
/// when awaiting it in a coroutine.
/// If constructed from a future, co_await-ing it will bypass
/// checking if the task quota is depleted, which means that
/// a ready future will be handled immediately.
template<typename T> struct [[nodiscard]] without_preemption_check : public seastar::future<T> {
    explicit without_preemption_check(seastar::future<T>&& f) noexcept : seastar::future<T>(std::move(f)) {}
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
template<typename T>
auto operator co_await(coroutine::without_preemption_check<T> f) noexcept {
    return internal::awaiter<false, T>(std::move(f));
}


} // seastar


namespace std {

template<typename T, typename... Args>
class coroutine_traits<seastar::future<T>, Args...> : public seastar::internal::coroutine_traits_base<T> {
};

} // std

