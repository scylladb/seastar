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

#include "core/future-util.hh"

/// \addtogroup fiber-module
/// @{

/// Exception thrown when a condition variable is broken by
/// \ref condition_variable::broken().
class broken_condition_variable : public std::exception {
public:
    /// Reports the exception reason.
    virtual const char* what() const noexcept {
        return "Conditional variable is broken";
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

/// FIXME: Support multiple waiters
class condition_variable {
    promise<> _p;
    bool _signaled = false;
    bool _broken = false;
public:
    /// Waits until condition variable is signaled, may wake up without condition been met
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken(), may contain an exception.
    future<> wait() {
        if (_broken) {
            return make_exception_future<>(broken_condition_variable());
        }
        if (_signaled) {
            _p = promise<>();
            _signaled = false;
            return make_ready_future<>();
        }
        return _p.get_future().then([this] {
            return wait();
        });
    }
    /// Waits until condition variable is notified and pred() == true, otherwise
    /// wait again.
    ///
    /// \return a future that becomes ready when \ref signal() is called
    ///         If the condition variable was \ref broken(), may contain an exception.
    template<typename Pred>
    future<> wait(Pred&& pred) {
        return do_until(std::forward<Pred>(pred), [this] {
            return wait();
        });
    }
    /// Notify variable and wake up a waiter if there is one
    void signal() {
        if (!_signaled) {
            _p.set_value();
            _signaled = true;
        }
    }
    /// Signal to waiters that an error occurred.  \ref wait() will see
    /// an exceptional future<> containing the provided exception parameter.
    /// The future is made available immediately.
    void broken() {
        _broken = true;
        signal();
    }
};

/// @}
