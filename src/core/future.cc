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
 * Copyright (C) 2020 ScyllaDB
 */

#include <seastar/core/future.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/report_exception.hh>
#include <seastar/util/backtrace.hh>

namespace seastar {
namespace internal {

promise_base::promise_base(promise_base&& x) noexcept
    : _future(x._future), _state(x._state), _task(std::exchange(x._task, nullptr)) {
    x._state = nullptr;
    if (auto* fut = _future) {
        fut->detach_promise();
        fut->_promise = this;
    }
}

promise_base::~promise_base() noexcept {
    if (_future) {
        assert(_state);
        assert(_state->available() || !_task);
        _future->detach_promise();
    } else if (__builtin_expect(bool(_task), false)) {
        assert(_state && !_state->available());
        _state->set_to_broken_promise();
        ::seastar::schedule(std::exchange(_task, nullptr));
    }
}

template <promise_base::urgent Urgent>
void promise_base::make_ready() noexcept {
    if (_task) {
        _state = nullptr;
        if (Urgent == urgent::yes && !need_preempt()) {
            ::seastar::schedule_urgent(std::exchange(_task, nullptr));
        } else {
            ::seastar::schedule(std::exchange(_task, nullptr));
        }
    }
}

template void promise_base::make_ready<promise_base::urgent::no>() noexcept;
template void promise_base::make_ready<promise_base::urgent::yes>() noexcept;

template
future<> internal::current_exception_as_future() noexcept;
}

/**
 * engine_exit() exits the reactor. It should be given a pointer to the
 * exception which prompted this exit - or a null pointer if the exit
 * request was not caused by any exception.
 */
void engine_exit(std::exception_ptr eptr) {
    if (!eptr) {
        engine().exit(0);
        return;
    }
    report_exception("Exiting on unhandled exception", eptr);
    engine().exit(1);
}

broken_promise::broken_promise() : logic_error("broken promise") { }

future_state_base future_state_base::current_exception() {
    return future_state_base(std::current_exception());
}

void future_state_base::set_to_broken_promise() noexcept {
    try {
        // Constructing broken_promise may throw (std::logic_error ctor is not noexcept).
        set_exception(std::make_exception_ptr(broken_promise{}));
    } catch (...) {
        set_exception(std::current_exception());
    }
}

void future_state_base::ignore() noexcept {
    switch (_u.st) {
    case state::invalid:
    case state::future:
        assert(0 && "invalid state for ignore");
    case state::result_unavailable:
    case state::result:
        _u.st = state::result_unavailable;
        break;
    default:
        // Ignore the exception
        _u.take_exception();
    }
}

void report_failed_future(const std::exception_ptr& eptr) noexcept {
    ++engine()._abandoned_failed_futures;
    seastar_logger.warn("Exceptional future ignored: {}, backtrace: {}", eptr, current_backtrace());
}

void report_failed_future(const future_state_base& state) noexcept {
    report_failed_future(state._u.ex);
}

}
