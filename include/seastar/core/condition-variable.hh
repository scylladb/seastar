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
 * Copyright (C) 2016 ScyllaDB, Ltd.
 */

#pragma once

#include <boost/intrusive/list.hpp>

#include <seastar/core/timer.hh>
#ifdef SEASTAR_COROUTINES_ENABLED
#   include <seastar/core/coroutine.hh>
#endif
#include <seastar/core/loop.hh>

namespace seastar {

/// \addtogroup fiber-module
/// @{

/// Exception thrown when a condition variable is broken by
/// \ref condition_variable::broken().
class broken_condition_variable : public std::exception {
public:
    /// Reports the exception reason.
    virtual const char* what() const noexcept;
};

/// Exception thrown when wait() operation times out
/// \ref condition_variable::wait(time_point timeout).
class condition_variable_timed_out : public std::exception {
public:
    /// Reports the exception reason.
    virtual const char* what() const noexcept;
};

/// \brief Conditional variable.
///
/// This is a standard computer science condition variable sans locking,
/// since in seastar access to variables is atomic anyway, adapted
/// for futures.  You can wait for variable to be notified.
///
/// To support exceptional conditions, a \ref broken() method
/// is provided, which causes all current waiters to stop waiting,
/// with an exceptional future returned.  This allows causing all
/// fibers that are blocked on a condition variable to continue.
/// This issimilar to POSIX's `pthread_cancel()`, with \ref wait()
/// acting as a cancellation point.

class condition_variable {
private:
    // the base for queue waiters. looks complicated, but this is
    // to make it transparent once we add non-promise based nodes
    struct waiter : public boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>> {
        virtual ~waiter() = default;
        void timeout() noexcept;

        virtual void signal() noexcept = 0;
        virtual void set_exception(std::exception_ptr) noexcept = 0;
    };

    struct promise_waiter : public waiter, public promise<> {
        void signal() noexcept override {
            set_value();
            // note: we self-delete in either case we are woken
            // up. See usage below: only the resulting future
            // state is required once we've left the wait queue
            delete this;
        }
        void set_exception(std::exception_ptr ep) noexcept override {
            promise<>::set_exception(std::move(ep));
            // see comment above
            delete this;
        }
    };

#ifdef SEASTAR_COROUTINES_ENABLED
    struct [[nodiscard("must co_await a when() call")]] awaiter : public waiter, private seastar::task {
        using handle_type = std::coroutine_handle<void>;

        condition_variable* _cv;
        handle_type _when_ready;
        std::exception_ptr _ex;
        task* _waiting_task = nullptr;

        awaiter(condition_variable* cv)
            : _cv(cv)
        {}

        bool await_ready() const {
            return _cv->check_and_consume_signal();
        }
        template<typename T>
        void await_suspend(std::coroutine_handle<T> h) {
            _when_ready = h;
            _waiting_task = &h.promise();
            _cv->add_waiter(*this);
        }
        void run_and_dispose() noexcept override {
            _when_ready.resume();
        }
        task* waiting_task() noexcept override { 
            return _waiting_task;
        }
        void await_resume() {
            if (_ex) {
                std::rethrow_exception(std::move(_ex));
            }
        }
        void signal() noexcept override {
            schedule(this);
        }
        void set_exception(std::exception_ptr ep) noexcept override {
            _ex = std::move(ep);
            schedule(this);
        }
    };

    template<typename Clock, typename Duration>
    struct [[nodiscard("must co_await a when() call")]] timeout_awaiter : public awaiter, public timer<Clock> {
        using my_type = timeout_awaiter<Clock, Duration>;
        using time_point = std::chrono::time_point<Clock, Duration>;

        time_point _timeout;

        timeout_awaiter(condition_variable* cv, time_point timeout)
            : awaiter(cv)
            , _timeout(timeout)
        {}
        template<typename T>
        void await_suspend(std::coroutine_handle<T> h) {
            awaiter::await_suspend(std::move(h));
            this->set_callback(std::bind(&waiter::timeout, this));
            this->arm(_timeout);
        }
        void signal() noexcept override {
            this->cancel();
            awaiter::signal();
        }
        void set_exception(std::exception_ptr ep) noexcept override {
            this->cancel();
            awaiter::set_exception(std::move(ep));
        }
    };

    template<typename Func, typename Base>
    struct [[nodiscard("must co_await a when() call")]] predicate_awaiter : public Base {
        Func _func;
        template<typename... Args>
        predicate_awaiter(Func func, Args&& ...args)
            : Base(std::forward<Args>(args)...)
            , _func(std::move(func))
        {}
        bool await_ready() const {
            if (!_func()) {
                Base::await_ready(); // clear out any signal state
                return false;
            }
            return true;
        }        
        void signal() noexcept override {
            if (Base::_ex || _func()) {
                Base::signal();
            } else {
                // must re-enter waiter queue
                // this maintains "wait" version
                // semantics of moving to back of queue
                // if predicate fails
                Base::_cv->add_waiter(*this);
            }
        }
    };
#endif

    boost::intrusive::list<waiter, boost::intrusive::constant_time_size<false>> _waiters;
    std::exception_ptr _ex; //"broken" exception
    bool _signalled = false; // set to true if signalled while no waiters

    void add_waiter(waiter&) noexcept;
    void timeout(waiter&) noexcept;
    bool wakeup_first() noexcept;
    bool check_and_consume_signal() noexcept;
public:
    /// Constructs a condition_variable object.
    /// Initialzie the semaphore with a default value of 0 to enusre
    /// the first call to wait() before signal() won't be waken up immediately.
    condition_variable() noexcept = default;
    condition_variable(condition_variable&& rhs) noexcept = default;
    ~condition_variable();

    /// Waits until condition variable is signaled, may wake up without condition been met
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken() will return \ref broken_condition_variable
    ///         exception.
    future<> wait() noexcept {
        if (check_and_consume_signal()) {
            return make_ready_future();
        }
        auto* w = new promise_waiter;
        auto f = w->get_future();
        add_waiter(*w);
        return f;
    }

    /// Waits until condition variable is signaled or timeout is reached
    ///
    /// \param timeout time point at which wait will exit with a timeout
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken() will return \ref broken_condition_variable
    ///         exception. If timepoint is reached will return \ref condition_variable_timed_out exception.
    template<typename Clock = typename timer<>::clock, typename Duration = typename Clock::duration>
    future<> wait(std::chrono::time_point<Clock, Duration> timeout) noexcept {
        if (check_and_consume_signal()) {
            return make_ready_future();
        }
        struct timeout_waiter : public promise_waiter, public timer<Clock> {};

        auto w = std::make_unique<timeout_waiter>();
        auto f = w->get_future();

        w->set_callback(std::bind(&waiter::timeout, w.get()));
        w->arm(timeout);
        add_waiter(*w.release());
        return f;
    }

    /// Waits until condition variable is signaled or timeout is reached
    ///
    /// \param timeout duration after which wait will exit with a timeout
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken() will return \ref broken_condition_variable
    ///         exception. If timepoint is passed will return \ref condition_variable_timed_out exception.
    template<typename Rep, typename Period>
    future<> wait(std::chrono::duration<Rep, Period> timeout) noexcept {
        return wait(timer<>::clock::now() + timeout);
    }

    /// Waits until condition variable is notified and pred() == true, otherwise
    /// wait again.
    ///
    /// \param pred predicate that checks that awaited condition is true
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken(), may contain an exception.
    template<typename Pred>
    SEASTAR_CONCEPT( requires seastar::InvokeReturns<Pred, bool> )
    future<> wait(Pred&& pred) noexcept {
        return do_until(std::forward<Pred>(pred), [this] {
            return wait();
        });
    }

    /// Waits until condition variable is notified and pred() == true or timeout is reached, otherwise
    /// wait again.
    ///
    /// \param timeout time point at which wait will exit with a timeout
    /// \param pred predicate that checks that awaited condition is true
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken() will return \ref broken_condition_variable
    ///         exception. If timepoint is reached will return \ref condition_variable_timed_out exception.
    template<typename Clock = typename timer<>::clock, typename Duration = typename Clock::duration, typename Pred>
    SEASTAR_CONCEPT( requires seastar::InvokeReturns<Pred, bool> )
    future<> wait(std::chrono::time_point<Clock, Duration> timeout, Pred&& pred) noexcept {
        return do_until(std::forward<Pred>(pred), [this, timeout] {
            return wait(timeout);
        });
    }

    /// Waits until condition variable is notified and pred() == true or timeout is reached, otherwise
    /// wait again.
    ///
    /// \param timeout duration after which wait will exit with a timeout
    /// \param pred predicate that checks that awaited condition is true
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken() will return \ref broken_condition_variable
    ///         exception. If timepoint is passed will return \ref condition_variable_timed_out exception.
    template<typename Rep, typename Period, typename Pred>
    SEASTAR_CONCEPT( requires seastar::InvokeReturns<Pred, bool> )
    future<> wait(std::chrono::duration<Rep, Period> timeout, Pred&& pred) noexcept {
        return wait(timer<>::clock::now() + timeout, std::forward<Pred>(pred));
    }

#ifdef SEASTAR_COROUTINES_ENABLED
    /// Coroutine/co_await only waiter.
    /// Waits until condition variable is signaled, may wake up without condition been met
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken() will return \ref broken_condition_variable
    ///         exception.
    awaiter when() noexcept {
        return awaiter{this};
    }

    /// Coroutine/co_await only waiter.
    /// Waits until condition variable is signaled or timeout is reached
    ///
    /// \param timeout time point at which wait will exit with a timeout
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken() will return \ref broken_condition_variable
    ///         exception. If timepoint is reached will return \ref condition_variable_timed_out exception.
    template<typename Clock = typename timer<>::clock, typename Duration = typename Clock::duration>
    timeout_awaiter<Clock, Duration> when(std::chrono::time_point<Clock, Duration> timeout) noexcept {
        return timeout_awaiter<Clock, Duration>{this, timeout};
    }

    /// Coroutine/co_await only waiter.
    /// Waits until condition variable is signaled or timeout is reached
    ///
    /// \param timeout duration after which wait will exit with a timeout
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken() will return \ref broken_condition_variable
    ///         exception. If timepoint is passed will return \ref condition_variable_timed_out exception.
    template<typename Rep, typename Period>
    auto when(std::chrono::duration<Rep, Period> timeout) noexcept {
        return when(timer<>::clock::now() + timeout);
    }

    /// Coroutine/co_await only waiter.
    /// Waits until condition variable is notified and pred() == true, otherwise
    /// wait again.
    ///
    /// \param pred predicate that checks that awaited condition is true
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken(), may contain an exception.
    template<typename Pred>
    SEASTAR_CONCEPT( requires seastar::InvokeReturns<Pred, bool> )
    auto when(Pred&& pred) noexcept {
        return predicate_awaiter<Pred, awaiter>{std::forward<Pred>(pred), when()};
    }

    /// Coroutine/co_await only waiter.
    /// Waits until condition variable is notified and pred() == true or timeout is reached, otherwise
    /// wait again.
    ///
    /// \param timeout time point at which wait will exit with a timeout
    /// \param pred predicate that checks that awaited condition is true
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken() will return \ref broken_condition_variable
    ///         exception. If timepoint is reached will return \ref condition_variable_timed_out exception.
    template<typename Clock = typename timer<>::clock, typename Duration = typename Clock::duration, typename Pred>
    SEASTAR_CONCEPT( requires seastar::InvokeReturns<Pred, bool> )
    auto when(std::chrono::time_point<Clock, Duration> timeout, Pred&& pred) noexcept {
        return predicate_awaiter<Pred, timeout_awaiter<Clock, Duration>>{std::forward<Pred>(pred), when(timeout)};
    }

    /// Coroutine/co_await only waiter.
    /// Waits until condition variable is notified and pred() == true or timeout is reached, otherwise
    /// wait again.
    ///
    /// \param timeout duration after which wait will exit with a timeout
    /// \param pred predicate that checks that awaited condition is true
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken() will return \ref broken_condition_variable
    ///         exception. If timepoint is passed will return \ref condition_variable_timed_out exception.
    template<typename Rep, typename Period, typename Pred>
    SEASTAR_CONCEPT( requires seastar::InvokeReturns<Pred, bool> )
    auto when(std::chrono::duration<Rep, Period> timeout, Pred&& pred) noexcept {
        return when(timer<>::clock::now() + timeout, std::forward<Pred>(pred));
    }

#endif

    /// Whether or not the condition variable currently has pending waiter(s)
    /// The returned answer is valid until next continuation/fiber switch.
    bool has_waiters() const noexcept {
        return !_waiters.empty();
    }

    /// Notify variable and wake up a waiter if there is one
    void signal() noexcept;

    /// Notify variable and wake up all waiter
    void broadcast() noexcept;

    /// Signal to waiters that an error occurred.  \ref wait() will see
    /// an exceptional future<> containing the provided exception parameter.
    /// The future is made available immediately.
    void broken() noexcept;

    void broken(std::exception_ptr) noexcept;
};

/// @}

}
