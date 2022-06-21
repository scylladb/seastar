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
 * Copyright (C) 2017 ScyllaDB
 */

#include <seastar/core/future-util.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/print.hh>
#include <seastar/core/semaphore.hh>

namespace seastar {

parallel_for_each_state::parallel_for_each_state(size_t n) {
    _incomplete.reserve(n);
}

future<> parallel_for_each_state::get_future() {
    auto ret = _result.get_future();
    wait_for_one();
    return ret;
}

void parallel_for_each_state::add_future(future<>&& f) {
    _incomplete.push_back(std::move(f));
}

void parallel_for_each_state::wait_for_one() noexcept {
    // Process from back to front, on the assumption that the front
    // futures are likely to complete earlier than the back futures.
    // If that's indeed the case, then the front futures will be
    // available and we won't have to wait for them.

    // Skip over futures that happen to be complete already.
    while (!_incomplete.empty() && _incomplete.back().available()) {
        if (_incomplete.back().failed()) {
            _ex = _incomplete.back().get_exception();
        }
        _incomplete.pop_back();
    }

    // If there's an incompelete future, wait for it.
    if (!_incomplete.empty()) {
        internal::set_callback(std::move(_incomplete.back()), static_cast<continuation_base<>*>(this));
        // This future's state will be collected in run_and_dispose(), so we can drop it.
        _incomplete.pop_back();
        return;
    }

    // Everything completed, report a result.
    if (__builtin_expect(bool(_ex), false)) {
        _result.set_exception(std::move(_ex));
    } else {
        _result.set_value();
    }
    delete this;
}

void parallel_for_each_state::run_and_dispose() noexcept {
    if (_state.failed()) {
        _ex = std::move(_state).get_exception();
    }
    _state = {};
    wait_for_one();
}

template <typename Clock>
future<> sleep_abortable(typename Clock::duration dur) {
    return engine().wait_for_stop(dur).then([] {
        throw sleep_aborted();
    }).handle_exception([] (std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch(condition_variable_timed_out&) {};
    });
}

template future<> sleep_abortable<steady_clock_type>(typename steady_clock_type::duration);
template future<> sleep_abortable<lowres_clock>(typename lowres_clock::duration);

template <typename Clock>
future<> sleep_abortable(typename Clock::duration dur, abort_source& as) {
    struct sleeper {
        promise<> done;
        timer<Clock> tmr;
        abort_source::subscription sc;

        sleeper(typename Clock::duration dur, abort_source& as)
                : tmr([this] { done.set_value(); }) {
            auto sc_opt = as.subscribe([this] (const std::optional<std::exception_ptr>& opt_ex) noexcept {
                if (tmr.cancel()) {
                    done.set_exception(opt_ex.value_or(std::make_exception_ptr(sleep_aborted())));
                }
            });
            if (sc_opt) {
                sc = std::move(*sc_opt);
                tmr.arm(dur);
            } else {
                done.set_exception(sleep_aborted());
            }
        }
    };
    //FIXME: Use do_with() after #373
    auto s = std::make_unique<sleeper>(dur, as);
    auto fut = s->done.get_future();
    return fut.finally([s = std::move(s)] { });
}

template future<> sleep_abortable<steady_clock_type>(typename steady_clock_type::duration, abort_source&);
template future<> sleep_abortable<lowres_clock>(typename lowres_clock::duration, abort_source&);

}
