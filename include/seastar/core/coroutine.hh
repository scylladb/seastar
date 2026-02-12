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

    // LIFO order
    std::forward_list<finally_function> _finally_funcs;
    coroutine_promise_base* _prev{};

private:
    virtual std::exception_ptr get_exception() noexcept = 0;
    virtual void set_promise_exception(std::exception_ptr&&) noexcept = 0;
    virtual void set_promise_value() noexcept = 0;

    void resume_after_finally(std::exception_ptr finally_ex) {
        if (auto ex = combine_exceptions(std::move(finally_ex), get_exception()); ex) {
            set_promise_exception(std::move(ex));
        } else {
            set_promise_value();
        }
    }

public:
    void push_coroutine();
    void pop_coroutine();

    void push_finally_function(finally_function&& func) {
        _finally_funcs.emplace_front(std::move(func));
    }
};

template <typename U>
void destroy_coroutine(seastar::task& coroutine_task) {
    auto promise_ptr = static_cast<U*>(&coroutine_task);
    auto hndl = std::coroutine_handle<U>::from_promise(*promise_ptr);
    hndl.destroy();
}

class final_awaiter : public seastar::task {
    coroutine_promise_base& _promise;

    seastar::future<> _future;
    std::exception_ptr _ex;

    seastar::task* _coroutine_task{};
    seastar::task* _waiting_task{};
    void (*_destroy_coroutine)(seastar::task&){};

private:
    void execute_one() {
        _future = _promise._finally_funcs.front()(_promise.get_exception());
        _future.set_coroutine(*this);
    }

public:
    explicit final_awaiter(coroutine_promise_base& promise)
        : _promise(promise)
        , _future(seastar::make_ready_future<>())
    { }
    final_awaiter(final_awaiter&&) = default;
    final_awaiter(const final_awaiter&) = delete;

    bool await_ready() const noexcept { return _promise._finally_funcs.empty(); }

    template <typename U>
    void await_suspend(std::coroutine_handle<U> hndl) noexcept {
        _coroutine_task = &hndl.promise();
        _waiting_task = hndl.promise().waiting_task();
        _destroy_coroutine = destroy_coroutine<U>;

        execute_one();
    }

    void await_resume() noexcept {
        _promise.resume_after_finally({});
    }

    virtual void run_and_dispose() noexcept final override {
        if (_future.failed()) {
            _ex = combine_exceptions(std::move(_ex), _future.get_exception());
        }

        _promise._finally_funcs.pop_front();

        if (!_promise._finally_funcs.empty()) {
            execute_one();
        } else {
            _promise.resume_after_finally(std::move(_ex));
            _destroy_coroutine(*_coroutine_task);
        }
    }

    virtual task* waiting_task() noexcept final override {
        return _waiting_task;
    }
};

template <typename T = void>
class coroutine_traits_base {
public:
    class promise_type final : public coroutine_promise_base, public coroutine_allocators {
        seastar::promise<T> _promise;
        std::variant<std::monostate, T, std::exception_ptr> _result;

    private:
        virtual std::exception_ptr get_exception() noexcept final override {
            if (auto ex = std::get_if<std::exception_ptr>(&_result); ex) {
                return *ex;
            }
            return {};
        }
        virtual void set_promise_exception(std::exception_ptr&& ex) noexcept final override {
            _promise.set_exception(std::move(ex));
        }
        virtual void set_promise_value() noexcept final override {
            _promise.set_value(std::move(std::get<T>(std::move(_result))));
        }

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

    private:
        virtual std::exception_ptr get_exception() noexcept final override {
            return _ex;
        }
        virtual void set_promise_exception(std::exception_ptr&& ex) noexcept final override {
            _promise.set_exception(std::move(ex));
        }
        virtual void set_promise_value() noexcept final override {
            _promise.set_value();
        }

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

