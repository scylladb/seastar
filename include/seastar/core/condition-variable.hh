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

#include <seastar/core/future-util.hh>
#include <seastar/core/semaphore.hh>

namespace seastar {

/// \addtogroup fiber-module
/// @{

/// Exception thrown when a condition variable is broken by
/// \ref condition_variable::broken().
class broken_condition_variable : public std::exception {
public:
    /// Reports the exception reason.
    virtual const char* what() const noexcept {
        return "Condition variable is broken";
    }
};

/// Exception thrown when wait() operation times out
/// \ref condition_variable::wait(time_point timeout).
class condition_variable_timed_out : public std::exception {
public:
    /// Reports the exception reason.
    virtual const char* what() const noexcept {
        return "Condition variable timed out";
    }
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
    using duration = semaphore::duration;
    using clock = semaphore::clock;
    using time_point = semaphore::time_point;
    struct condition_variable_exception_factory {
        static condition_variable_timed_out timeout() {
            return condition_variable_timed_out();
        }
        static broken_condition_variable broken() {
            return broken_condition_variable();
        }
    };
    basic_semaphore<condition_variable_exception_factory> _sem;
public:
    /// Constructs a condition_variable object.
    /// Initialzie the semaphore with a default value of 0 to enusre
    /// the first call to wait() before signal() won't be waken up immediately.
    condition_variable() : _sem(0) {}

    /// Waits until condition variable is signaled, may wake up without condition been met
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken() will return \ref broken_condition_variable
    ///         exception.
    future<> wait() {
        return _sem.wait();
    }

    /// Waits until condition variable is signaled or timeout is reached
    ///
    /// \param timeout time point at which wait will exit with a timeout
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken() will return \ref broken_condition_variable
    ///         exception. If timepoint is reached will return \ref condition_variable_timed_out exception.
    future<> wait(time_point timeout) {
        return _sem.wait(timeout);
    }

    /// Waits until condition variable is signaled or timeout is reached
    ///
    /// \param timeout duration after which wait will exit with a timeout
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken() will return \ref broken_condition_variable
    ///         exception. If timepoint is passed will return \ref condition_variable_timed_out exception.
    future<> wait(duration timeout) {
        return _sem.wait(timeout);
    }

    /// Waits until condition variable is notified and pred() == true, otherwise
    /// wait again.
    ///
    /// \param pred predicate that checks that awaited condition is true
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken(), may contain an exception.
    template<typename Pred>
    future<> wait(Pred&& pred) {
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
    /// \param
    template<typename Pred>
    future<> wait(time_point timeout, Pred&& pred) {
        return do_until(std::forward<Pred>(pred), [this, timeout] () mutable {
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
    template<typename Pred>
    future<> wait(duration timeout, Pred&& pred) {
        return wait(clock::now() + timeout, std::forward<Pred>(pred));
    }
    /// Notify variable and wake up a waiter if there is one
    void signal() {
        if (_sem.waiters()) {
            _sem.signal();
        }
    }
    /// Notify variable and wake up all waiter
    void broadcast() {
        _sem.signal(_sem.waiters());
    }

    /// Signal to waiters that an error occurred.  \ref wait() will see
    /// an exceptional future<> containing the provided exception parameter.
    /// The future is made available immediately.
    void broken() {
        _sem.broken();
    }
};

/// @}

}
