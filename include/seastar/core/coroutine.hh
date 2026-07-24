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
#if defined(__GNUC__) && !defined(__clang__) && (__GNUC__ == 15 && __GNUC_MINOR__ == 2)
    memory::scoped_critical_alloc_section _;
    schedule(new lambda_task(current_scheduling_group(), std::forward<decltype(func)>(func)));
#else
    std::invoke(std::forward<decltype(func)>(func));
#endif
}

inline
void
execute_involving_handle_destruction_in_await_suspend(task* tsk) noexcept {
#if defined(__GNUC__) && !defined(__clang__) && (__GNUC__ == 15 && __GNUC_MINOR__ == 2)
    schedule(tsk);
#else
    tsk->run_and_dispose();
#endif
}

// Defined in reactor.cc; declared here to avoid pulling in the heavy
// reactor.hh from this widely-included header.
void set_current_task(task* t);

// Transfer control from a just-completed coroutine to the task waiting on it.
//
// `self` is the handle of the completing coroutine; its frame is destroyed here
// (the frame is self-owned, so it must dispose of itself before transferring).
// If the waiter is itself a coroutine in the current scheduling group and the
// task quota is not depleted, resume it directly via symmetric transfer,
// bypassing the reactor's task queue; the returned handle is then resumed by the
// coroutine machinery as a tail call. Otherwise fall back to scheduling the
// waiter through the reactor and return the no-op handle.
inline std::coroutine_handle<> coroutine_return_to_waiter(task* waiter, std::coroutine_handle<> self) noexcept {
    if (waiter) {
        void* addr = waiter->to_coroutine_handle_address();
        if (addr && waiter->group() == current_scheduling_group() && !need_preempt()) {
            // The reactor's current-task pointer still refers to `self`, whose
            // frame we are about to destroy; repoint it at the waiter so that
            // e.g. stall backtraces taken while the waiter runs stay valid.
            set_current_task(waiter);
            self.destroy();
            return std::coroutine_handle<>::from_address(addr);
        }
        schedule(waiter);
    }
    self.destroy();
    return std::noop_coroutine();
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

template <typename T = void>
class coroutine_traits_base {
public:
    class promise_type final : public seastar::task, public coroutine_allocators {
        seastar::promise<T> _promise;
        // Task waiting for this coroutine to complete, captured out of _promise
        // before its state is made ready so that the regular ready path does not
        // schedule it; final_suspend() then resumes it via symmetric transfer or
        // schedules it explicitly. See final_awaiter.
        task* _waiter = nullptr;

        // Remove the waiting task from _promise without scheduling it, so that
        // setting the promise's value below leaves the waiter to be resumed by
        // final_suspend(). Call this on synchronous completion paths, just before
        // the value or exception is set. It must not be used on paths that resolve
        // the promise asynchronously (e.g. forward_to()), which need the regular
        // scheduling machinery to remain in charge of the waiter.
        void capture_waiter() noexcept { _waiter = _promise.take_waiting_task(); }

        struct final_awaiter {
            task* waiter;
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                return coroutine_return_to_waiter(waiter, h);
            }
            void await_resume() noexcept {}
        };
    public:
        promise_type() = default;
        promise_type(promise_type&&) = delete;
        promise_type(const promise_type&) = delete;

#if SEASTAR_API_LEVEL < 10
        template<typename U>
        void return_value(U&& value) {
            capture_waiter();
            _promise.set_value(std::forward<U>(value));
        }

        [[deprecated("Forwarding coroutine returns are deprecated as too dangerous. Use 'co_return co_await ...' until explicit syntax is available.")]]
        void return_value(future<T>&& fut) noexcept {
            // Asynchronous resolution: leave the waiter attached to _promise so
            // that forward_to()'s completion resumes it through the reactor.
            fut.forward_to(std::move(_promise));
        }
#else
        // this non-templated version only exists to support co_returning a braced-init-list
        void return_value(T&& value) noexcept {
            capture_waiter();
            _promise.set_value(std::forward<T>(value));
        }

        template<typename U>
        requires std::is_convertible_v<U&&, T>
        void return_value(U&& value) noexcept {
            capture_waiter();
            _promise.set_value(std::forward<U>(value));
        }
#endif

        void return_value(coroutine::exception ce) noexcept {
            capture_waiter();
            _promise.set_exception(std::move(ce.eptr));
        }

        // Called by the exception/try_future awaiters, which resolve the promise
        // and destroy the frame directly without reaching final_suspend(). It must
        // therefore leave the waiter attached so the regular ready path schedules
        // it; do not capture_waiter() here.
        //
        // As a result these paths do not use symmetric transfer; extending them to
        // do so (in particular try_future) is left to a later change.
        void set_exception(std::exception_ptr&& eptr) noexcept {
            _promise.set_exception(std::move(eptr));
        }

        void unhandled_exception() noexcept {
            capture_waiter();
            _promise.set_exception(std::current_exception());
        }

        seastar::future<T> get_return_object() noexcept {
            return _promise.get_future();
        }

        std::suspend_never initial_suspend() noexcept { return { }; }
        final_awaiter final_suspend() noexcept { return final_awaiter{_waiter}; }

        virtual void run_and_dispose() noexcept override {
            auto handle = std::coroutine_handle<promise_type>::from_promise(*this);
            handle.resume();
        }

        task* waiting_task() noexcept override { return _promise.waiting_task(); }

        void* to_coroutine_handle_address() noexcept override {
            return std::coroutine_handle<promise_type>::from_promise(*this).address();
        }

        scheduling_group set_scheduling_group(scheduling_group sg) noexcept {
            return std::exchange(this->_sg, sg);
        }
    };
};

template <>
class coroutine_traits_base<> {
public:
   class promise_type final : public seastar::task, public coroutine_allocators {
        seastar::promise<> _promise;
        // See the equivalent members in coroutine_traits_base<T>::promise_type.
        task* _waiter = nullptr;

        void capture_waiter() noexcept { _waiter = _promise.take_waiting_task(); }

        struct final_awaiter {
            task* waiter;
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                return coroutine_return_to_waiter(waiter, h);
            }
            void await_resume() noexcept {}
        };
    public:
        promise_type() = default;
        promise_type(promise_type&&) = delete;
        promise_type(const promise_type&) = delete;

        void return_void() noexcept {
            capture_waiter();
            _promise.set_value();
        }

        // Called by the exception/try_future awaiters, which resolve the promise
        // and destroy the frame directly without reaching final_suspend(). It must
        // therefore leave the waiter attached so the regular ready path schedules
        // it; do not capture_waiter() here.
        //
        // As a result these paths do not use symmetric transfer; extending them to
        // do so (in particular try_future) is left to a later change.
        void set_exception(std::exception_ptr&& eptr) noexcept {
            _promise.set_exception(std::move(eptr));
        }

        void unhandled_exception() noexcept {
            capture_waiter();
            _promise.set_exception(std::current_exception());
        }

        seastar::future<> get_return_object() noexcept {
            return _promise.get_future();
        }

        std::suspend_never initial_suspend() noexcept { return { }; }
        final_awaiter final_suspend() noexcept { return final_awaiter{_waiter}; }

        virtual void run_and_dispose() noexcept override {
            auto handle = std::coroutine_handle<promise_type>::from_promise(*this);
            handle.resume();
        }

        task* waiting_task() noexcept override { return _promise.waiting_task(); }

        void* to_coroutine_handle_address() noexcept override {
            return std::coroutine_handle<promise_type>::from_promise(*this).address();
        }

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

