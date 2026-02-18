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
    at_exit_function _func;
    std::unique_ptr<at_exit_func_stack> _next;

public:
    at_exit_func_stack() = default;
    explicit at_exit_func_stack(at_exit_function&& func) : _func(std::move(func)) { }

    bool empty() const noexcept { return !_func; }

    at_exit_function& top() noexcept { return _func; }

    void push(at_exit_function&& func) {
        auto new_node = _func ? std::make_unique<at_exit_func_stack>(std::move(*this)) : nullptr;
        _func = std::move(func);
        _next = std::move(new_node);
    }

    void pop() noexcept {
        _func = {};
        if (auto next = std::exchange(_next, nullptr)) {
            new (this) at_exit_func_stack(std::move(*next));
        }
    }
};

inline std::exception_ptr combine_exceptions(std::exception_ptr ex1, std::exception_ptr ex2) {
    if (ex1 && ex2) {
        return std::make_exception_ptr(nested_exception(std::move(ex1), std::move(ex2)));
    } else if (ex1) {
        return ex1;
    } else {
        return ex2;
    }
}

class coroutine_promise_base : public seastar::task {
    friend class final_awaiter;

    at_exit_func_stack _at_exit_funcs;

private:
    virtual std::exception_ptr get_exception() noexcept = 0;
    virtual void resume_after_at_exit(std::exception_ptr at_exit_ex) noexcept = 0;

protected:
    template <typename T>
    void do_resume_after_at_exit(seastar::promise<T>&& proxy_promise, seastar::promise<T>&& return_promise, std::exception_ptr at_exit_ex) noexcept {
        auto proxy_fut = proxy_promise.get_future();
        if (at_exit_ex) {
            auto final_ex = combine_exceptions(std::move(at_exit_ex), proxy_promise.get_exception());
            proxy_fut.ignore_ready_future();
            proxy_fut = seastar::make_exception_future<T>(std::move(final_ex));
        }
        proxy_fut.forward_to(std::move(return_promise));
    }

    coroutine_promise_base() = default;
    ~coroutine_promise_base() {
        // Destroying a coroutine without running the at exit functions is a bug
        // in seastar and will almost certainly going to lead to data corruption,
        // due to missing cleanup.
        assert(_at_exit_funcs.empty());
    }

public:
    void push_coroutine();
    void pop_coroutine();

    void push_at_exit_function(at_exit_function&& func) {
        _at_exit_funcs.push(std::move(func));
    }
};

template <typename U>
void destroy_coroutine(seastar::task& coroutine_task) {
    auto promise_ptr = static_cast<U*>(&coroutine_task);
    auto hndl = std::coroutine_handle<U>::from_promise(*promise_ptr);
    hndl.destroy();
}

class final_awaiter final : public seastar::task {
    coroutine_promise_base& _promise;
    bool _self_destruct = false;

    seastar::future<> _future;
    std::exception_ptr _ex;

    seastar::task* _coroutine_task{};
    seastar::task* _waiting_task{};
    void (*_destroy_coroutine)(seastar::task&){};

private:
    void execute_one() noexcept {
        try {
            _future = _promise._at_exit_funcs.top()(_promise.get_exception());
            _future.set_coroutine(*this);
        } catch (...) {
            _future = seastar::make_exception_future<>(std::current_exception());
            run_and_dispose();
        }
    }

public:
    explicit final_awaiter(coroutine_promise_base& promise)
        : _promise(promise)
        , _future(seastar::make_ready_future<>())
    { }

    final_awaiter(final_awaiter&&) noexcept = default;
    final_awaiter(const final_awaiter&) = delete;

    bool await_ready() const noexcept { return _promise._at_exit_funcs.empty(); }

    template <typename U>
    void await_suspend(std::coroutine_handle<U> hndl) noexcept {
        _coroutine_task = &hndl.promise();
        _waiting_task = hndl.promise().waiting_task();
        _destroy_coroutine = destroy_coroutine<U>;

        execute_one();
    }

    void await_resume() noexcept {
        _promise.resume_after_at_exit({});
    }

    void self_destruct_when_done() noexcept {
        _self_destruct = true;
    }

    virtual void run_and_dispose() noexcept final override {
        if (_future.failed()) {
            _ex = combine_exceptions(std::move(_ex), _future.get_exception());
        }

        _promise._at_exit_funcs.pop();

        if (!_promise._at_exit_funcs.empty()) {
            execute_one();
        } else {
            // _destroy_coroutine() will destroy this object when the
            // final_awaiter is allocated on the coroutine frame. Save the value
            // of this member into a local variable before calling it.
            const auto self_destruct = _self_destruct;

            _promise.resume_after_at_exit(std::move(_ex));
            _destroy_coroutine(*_coroutine_task);

            // Handle the case when final_awaiter is not allocated on the coroutine frame, see
            // finalize_and_destroy_coroutine().
            if (self_destruct) {
                delete this;
            }
        }
    }

    virtual task* waiting_task() noexcept final override {
        return _waiting_task;
    }
};

// Does not implement the full awaiter control semantics described at
// https://en.cppreference.com/w/cpp/language/coroutines.html.
// It is enough if this is compatible with our promise_type<>::final_suspend().
template <typename U>
void finalize_and_destroy_coroutine(std::coroutine_handle<U> hndl) {
    auto final_suspend = std::make_unique<final_awaiter>(hndl.promise().final_suspend());
    if (final_suspend->await_ready()) {
        final_suspend->await_resume();
        hndl.destroy();
    } else {
        final_suspend->await_suspend(hndl);

        // Hand over destruction to final_awaiter::run_and_dispose()
        final_suspend->self_destruct_when_done();
        final_suspend.release();
    }
}

template <typename T = void>
class coroutine_traits_base {
public:
    class promise_type final : public coroutine_promise_base, public coroutine_allocators {
        seastar::promise<T> _return_promise;
        seastar::promise<T> _proxy_promise;

    private:
        virtual std::exception_ptr get_exception() noexcept final override {
            return _proxy_promise.get_exception();
        }
        virtual void resume_after_at_exit(std::exception_ptr at_exit_ex) noexcept final override {
            do_resume_after_at_exit(std::move(_proxy_promise), std::move(_return_promise), std::move(at_exit_ex));
        }

    public:
        promise_type() = default;
        promise_type(promise_type&&) = delete;
        promise_type(const promise_type&) = delete;

        template<typename... U>
        void return_value(U&&... value) {
            _proxy_promise.set_value(std::forward<U>(value)...);
        }

        void return_value(coroutine::exception ce) noexcept {
            _proxy_promise.set_exception(std::move(ce).eptr);
        }

        void set_exception(std::exception_ptr&& eptr) noexcept {
            _proxy_promise.set_exception(std::move(eptr));
        }

        [[deprecated("Forwarding coroutine returns are deprecated as too dangerous. Use 'co_return co_await ...' until explicit syntax is available.")]]
        void return_value(future<T>&& fut) noexcept {
            fut.forward_to(std::move(_proxy_promise));
        }

        void unhandled_exception() noexcept {
            _proxy_promise.set_to_current_exception();
        }

        seastar::future<T> get_return_object() noexcept {
            return _return_promise.get_future();
        }

        std::suspend_never initial_suspend() noexcept {
            push_coroutine();
            return { };
        }
        final_awaiter final_suspend() noexcept {
            pop_coroutine();
            return final_awaiter(*this);
        }

        virtual void run_and_dispose() noexcept override {
            push_coroutine();
            auto handle = std::coroutine_handle<promise_type>::from_promise(*this);
            handle.resume();
        }

        task* waiting_task() noexcept override { return _return_promise.waiting_task(); }

        scheduling_group set_scheduling_group(scheduling_group sg) noexcept {
            return std::exchange(this->_sg, sg);
        }
    };
};

template <>
class coroutine_traits_base<> {
public:
   class promise_type final : public coroutine_promise_base, public coroutine_allocators {
        seastar::promise<> _proxy_promise;
        seastar::promise<> _return_promise;

    private:
        virtual std::exception_ptr get_exception() noexcept final override {
            return _proxy_promise.get_exception();
        }
        virtual void resume_after_at_exit(std::exception_ptr at_exit_ex) noexcept final override {
            do_resume_after_at_exit(std::move(_proxy_promise), std::move(_return_promise), std::move(at_exit_ex));
        }

    public:
        promise_type() = default;
        promise_type(promise_type&&) = delete;
        promise_type(const promise_type&) = delete;

        void return_void() noexcept {
            _proxy_promise.set_value();
        }

        void set_exception(std::exception_ptr&& eptr) noexcept {
            _proxy_promise.set_exception(std::move(eptr));
        }

        void unhandled_exception() noexcept {
            _proxy_promise.set_to_current_exception();
        }

        seastar::future<> get_return_object() noexcept {
            return _return_promise.get_future();
        }

        std::suspend_never initial_suspend() noexcept {
            push_coroutine();
            return { };
        }
        final_awaiter final_suspend() noexcept {
            pop_coroutine();
            return final_awaiter(*this);
        }

        virtual void run_and_dispose() noexcept override {
            push_coroutine();
            auto handle = std::coroutine_handle<promise_type>::from_promise(*this);
            handle.resume();
        }

        task* waiting_task() noexcept override { return _return_promise.waiting_task(); }

        scheduling_group set_scheduling_group(scheduling_group new_sg) noexcept {
            return task::set_scheduling_group(new_sg);
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
    void await_suspend(std::coroutine_handle<U> hndl) noexcept {
        if constexpr (std::is_base_of_v<coroutine_promise_base, std::remove_cvref_t<decltype(hndl.promise())>>) {
            hndl.promise().pop_coroutine();
        }
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
    void await_suspend(std::coroutine_handle<U> hndl) noexcept {
        if constexpr (std::is_base_of_v<coroutine_promise_base, std::remove_cvref_t<decltype(hndl.promise())>>) {
            hndl.promise().pop_coroutine();
        }
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

