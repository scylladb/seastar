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
 * Copyright (C) 2020 ScyllaDB, Ltd.
 */
#ifdef SEASTAR_MODULE
module;
#include <exception>
#include <utility>
#include <memory>
module seastar;
#else
#include <seastar/core/condition-variable.hh>
#endif
#include <seastar/util/assert.hh>

namespace seastar {

const char* broken_condition_variable::what() const noexcept {
    return "Condition variable is broken";
}

const char* condition_variable_timed_out::what() const noexcept {
    return "Condition variable timed out";
}

condition_variable::~condition_variable() {
    broken();
}

void condition_variable::add_waiter(waiter& w) noexcept {
    SEASTAR_ASSERT(!_signalled); // should not have snuck between
    if (_ex) {
        w.set_exception(_ex);
        return;
    }
    _waiters.push_back(w);
}

void condition_variable::waiter::timeout() noexcept {
    this->unlink();
    this->set_exception(std::make_exception_ptr(condition_variable_timed_out()));
}

void condition_variable::waiter::abort(const std::exception_ptr& ex) noexcept {
    this->unlink();
    this->set_exception(ex);
}

bool condition_variable::wakeup_first() noexcept {
    if (_waiters.empty()) {
        return false;
    }
    auto& w = _waiters.front();
    _waiters.pop_front();
    if (_ex) {
        w.set_exception(_ex);
    } else {
        w.signal();
    }
    return true;
}

bool condition_variable::check_and_consume_signal() noexcept {
    return std::exchange(_signalled, false);
}

void condition_variable::signal() noexcept {
    if (!wakeup_first()) {
        _signalled = true;
    }
}

future<> condition_variable::wait(abort_source& as) noexcept {
    if (check_and_consume_signal()) {
        return make_ready_future();
    }
    struct abort_waiter : public promise_waiter {
        abort_source::subscription sub;
    };
    auto w = std::make_unique<abort_waiter>();
    auto sub = as.subscribe([w = w.get(), &as]() noexcept {
        w->abort(as.abort_requested_exception_ptr());
    });
    auto f = w->get_future();
    if (!sub) {
      w.release()->set_exception(as.abort_requested_exception_ptr());
      return f;
    }
    w->sub = std::move(*sub);
    add_waiter(*w.release());
    return f;
}

/// Notify variable and wake up all waiter
void condition_variable::broadcast() noexcept {
    auto tmp(std::move(_waiters));
    while (!tmp.empty()) {
        auto& w = tmp.front();
        tmp.pop_front();
        if (_ex) {
            w.set_exception(_ex);
        } else {
            w.signal();
        }
    }
}

/// Signal to waiters that an error occurred.  \ref wait() will see
/// an exceptional future<> containing the provided exception parameter.
/// The future is made available immediately.
void condition_variable::broken() noexcept {
    broken(std::make_exception_ptr(broken_condition_variable()));
}

void condition_variable::broken(std::exception_ptr ep) noexcept {
    _ex = ep;
    broadcast();
}

} // namespace seastar
