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

#ifndef SEASTAR_MODULE
#include <boost/intrusive/list.hpp>
#include <chrono>
#include <exception>
#include <functional>
#endif

#include <seastar/core/future.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/modules.hh>

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

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
        waiter() = default;
        waiter(waiter&&) = default;
        waiter(const waiter&) = delete;
        waiter& operator=(const waiter&) = delete;
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

    struct [[nodiscard("must co_await a when() call")]] awaiter : public waiter {
        condition_variable* _cv;
        promise<> _p;

        awaiter(condition_variable* cv)
            : _cv(cv)
        {}

        void signal() noexcept override {
            _p.set_value();
        }
        void set_exception(std::exception_ptr ep) noexcept override {
            _p.set_exception(std::move(ep));
        }
        auto operator co_await() {
            if (_cv->check_and_consume_signal()) {
                return ::seastar::internal::awaiter<false, void>(make_ready_future<>());
            }
            _cv->add_waiter(*this);
            return ::seastar::internal::awaiter<false, void>(_p.get_future());
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
        void signal() noexcept override {
            this->cancel();
            awaiter::signal();
        }
        void set_exception(std::exception_ptr ep) noexcept override {
            this->cancel();
            awaiter::set_exception(std::move(ep));
        }
        auto operator co_await() {
            if (_cv->check_and_consume_signal()) {
                return ::seastar::internal::awaiter<false, void>(make_ready_future<>());
            }
            this->set_callback(std::bind(&waiter::timeout, this));
            this->arm(_timeout);
            return awaiter::operator co_await();
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
        void signal() noexcept override {
            try {
                if (_func()) {
                    Base::signal();
                } else {
                    // must re-enter waiter queue
                    // this maintains "wait" version
                    // semantics of moving to back of queue
                    // if predicate fails
                    Base::_cv->add_waiter(*this);
                }
            } catch(...) {
                Base::set_exception(std::current_exception());
            }
        }
        auto operator co_await() {
            try {
                if (_func()) {
                    return ::seastar::internal::awaiter<false, void>(make_ready_future<>());
                } else {
                    Base::_cv->check_and_consume_signal(); // clear out any signal state
                    return Base::operator co_await();
                }
            } catch (...) {
                return ::seastar::internal::awaiter<false, void>(make_exception_future(std::current_exception()));
            }
        }
    };

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
    requires std::is_invocable_r_v<bool, Pred>
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
    requires std::is_invocable_r_v<bool, Pred>
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
    requires std::is_invocable_r_v<bool, Pred>
    future<> wait(std::chrono::duration<Rep, Period> timeout, Pred&& pred) noexcept {
        return wait(timer<>::clock::now() + timeout, std::forward<Pred>(pred));
    }

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
    requires std::is_invocable_r_v<bool, Pred>
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
    requires std::is_invocable_r_v<bool, Pred>
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
    requires std::is_invocable_r_v<bool, Pred>
    auto when(std::chrono::duration<Rep, Period> timeout, Pred&& pred) noexcept {
        return when(timer<>::clock::now() + timeout, std::forward<Pred>(pred));
    }

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

SEASTAR_MODULE_EXPORT_END

}
