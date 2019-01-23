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

#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>

namespace seastar {

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
            auto sc_opt = as.subscribe([this] {
                if (tmr.cancel()) {
                    done.set_exception(sleep_aborted());
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
