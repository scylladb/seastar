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
#include <seastar/util/log.hh>


#include <coroutine>
#include <new>
#include <cstdlib>
#include <forward_list>

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

using finally_function = noncopyable_function<future<>(std::exception_ptr)>;

class coroutine_promise_base : public seastar::task {
protected:
    // LIFO order
    std::forward_list<finally_function> _cleanup_funcs;
    coroutine_promise_base* _prev{};

public:
    void push();
    void pop();

    void push_cleanup_function(finally_function&& func) {
        _cleanup_funcs.emplace_front(std::move(func));
    }
};

template <typename U>
void destroy_coroutine(seastar::task& coroutine_task) {
    auto promise_ptr = static_cast<U*>(&coroutine_task);
    auto hndl = std::coroutine_handle<U>::from_promise(*promise_ptr);
    hndl.destroy();
}

class final_awaiter_base : public seastar::task {
    std::forward_list<finally_function>& _cleanup_funcs;
    std::exception_ptr _ex;

    seastar::future<> _future;
    std::exception_ptr _finally_ex;

    seastar::task* _coroutine_task{};
    seastar::task* _waiting_task{};
    void (*_destroy_coroutine)(seastar::task&){};

private:
    void execute_one() {
        _future = _cleanup_funcs.front()(_ex);
        _future.set_coroutine(*this);
    }

    std::exception_ptr combine_exceptions(std::exception_ptr ex1, std::exception_ptr ex2) {
        if (ex1 && ex2) {
            return std::make_exception_ptr(nested_exception(std::move(ex1), std::move(ex2)));
        } else if (ex1) {
            return ex1;
        } else {
            return ex2;
        }
    }

    virtual void resume_flow(std::exception_ptr combined_ex) = 0;

public:
    final_awaiter_base(std::forward_list<finally_function>& cleanup_funcs, std::exception_ptr ex)
        : _cleanup_funcs(cleanup_funcs)
        , _ex(ex)
        , _future(seastar::make_ready_future<>())
    { }
    final_awaiter_base(final_awaiter_base&&) = default;
    final_awaiter_base(const final_awaiter_base&) = default;

    bool await_ready() const noexcept { return _cleanup_funcs.empty(); }

    template <typename U>
    void await_suspend(std::coroutine_handle<U> hndl) noexcept {
        _coroutine_task = &hndl.promise();
        _waiting_task = hndl.promise().waiting_task();
        _destroy_coroutine = destroy_coroutine<U>;

        execute_one();
    }

    void await_resume() noexcept {
        resume_flow(std::move(_ex));
    }

    virtual void run_and_dispose() noexcept final override {
        if (_future.failed()) {
            _finally_ex = combine_exceptions(std::move(_finally_ex), _future.get_exception());
        }
        _cleanup_funcs.pop_front();

        if (_cleanup_funcs.empty()) {
            resume_flow(combine_exceptions(std::move(_ex), std::move(_finally_ex)));
            _destroy_coroutine(*_coroutine_task);
        } else {
            execute_one();
        }
    }

    virtual task* waiting_task() noexcept final override {
        return _waiting_task;
    }
};

template <typename T = void>
class final_awaiter final : public final_awaiter_base {
    promise<T>& _pr;
    std::variant<std::monostate, T, std::exception_ptr>& _result;

private:
    virtual void resume_flow(std::exception_ptr combined_ex) final override {
        if (combined_ex) {
            _pr.set_exception(std::move(combined_ex));
        } else {
            _pr.set_value(std::get<T>(std::move(_result)));
        }
    }
public:
    final_awaiter(std::forward_list<finally_function>& cleanup_funcs, promise<T>& pr, std::variant<std::monostate, T, std::exception_ptr>& result)
        : final_awaiter_base(cleanup_funcs, std::holds_alternative<std::exception_ptr>(result) ? std::get<std::exception_ptr>(result) : nullptr)
        , _pr(pr)
        , _result(result)
    { }
};

template <>
class final_awaiter<> final : public final_awaiter_base {
    promise<>& _pr;

private:
    virtual void resume_flow(std::exception_ptr combined_ex) final override {
        if (combined_ex) {
            _pr.set_exception(std::move(combined_ex));
        } else {
            _pr.set_value();
        }
    }

public:
    final_awaiter(std::forward_list<finally_function>& cleanup_funcs, promise<>& pr, std::exception_ptr ex)
        : final_awaiter_base(cleanup_funcs, std::move(ex))
        , _pr(pr)
    { }
};

template <typename T = void>
class coroutine_traits_base {
public:
    class promise_type final : public coroutine_promise_base, public coroutine_allocators {
        seastar::promise<T> _promise;
        std::variant<std::monostate, T, std::exception_ptr> _result;

    public:
        promise_type() = default;
        promise_type(promise_type&&) = delete;
        promise_type(const promise_type&) = delete;

        template<typename... U>
        void return_value(U&&... value) {
            _result.template emplace<T>(std::forward<U>(value)...);
        }

        void return_value(coroutine::exception ce) noexcept {
            _result.template emplace<std::exception_ptr>(std::move(ce.eptr));
        }

        void set_exception(std::exception_ptr&& eptr) noexcept {
            _result.template emplace<std::exception_ptr>(std::move(eptr));
        }

        [[deprecated("Forwarding coroutine returns are deprecated as too dangerous. Use 'co_return co_await ...' until explicit syntax is available.")]]
        void return_value(future<T>&& fut) noexcept {
            fut.forward_to(std::move(_promise));
        }

        void unhandled_exception() noexcept {
            _result.template emplace<std::exception_ptr>(std::current_exception());
        }

        seastar::future<T> get_return_object() noexcept {
            return _promise.get_future();
        }

        std::suspend_never initial_suspend() noexcept {
            push();
            return { };
        }
        final_awaiter<T> final_suspend() noexcept {
            pop();
            return final_awaiter<T>(_cleanup_funcs, _promise, _result);
        }

        virtual void run_and_dispose() noexcept override {
            push();
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
   class promise_type final : public coroutine_promise_base, public coroutine_allocators {
        seastar::promise<> _promise;
        std::exception_ptr _ex;

    public:
        promise_type() = default;
        promise_type(promise_type&&) = delete;
        promise_type(const promise_type&) = delete;

        void return_void() noexcept { }

        void set_exception(std::exception_ptr&& eptr) noexcept {
            _ex = std::move(eptr);
        }

        void unhandled_exception() noexcept {
            _ex = std::current_exception();
        }

        seastar::future<> get_return_object() noexcept {
            return _promise.get_future();
        }

        std::suspend_never initial_suspend() noexcept {
            push();
            return { };
        }
        final_awaiter<> final_suspend() noexcept {
            pop();
            return final_awaiter<>(_cleanup_funcs, _promise, _ex);
        }

        virtual void run_and_dispose() noexcept override {
            push();
            auto handle = std::coroutine_handle<promise_type>::from_promise(*this);
            handle.resume();
        }

        task* waiting_task() noexcept override { return _promise.waiting_task(); }

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
            hndl.promise().pop();
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

