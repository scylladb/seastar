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
 * Copyright 2015 Cloudius Systems
 */

#pragma once

#include <chrono>
#include <seastar/util/std-compat.hh>
#include <atomic>
#include <functional>
#include <seastar/core/future.hh>
#include <seastar/core/timer-set.hh>

namespace seastar {

using steady_clock_type = std::chrono::steady_clock;

template <typename Clock = steady_clock_type>
class timer {
public:
    typedef typename Clock::time_point time_point;
    typedef typename Clock::duration duration;
    typedef Clock clock;
private:
    using callback_t = noncopyable_function<void()>;
    boost::intrusive::list_member_hook<> _link;
    callback_t _callback;
    time_point _expiry;
    compat::optional<duration> _period;
    bool _armed = false;
    bool _queued = false;
    bool _expired = false;
    void readd_periodic();
    void arm_state(time_point until, compat::optional<duration> period) {
        assert(!_armed);
        _period = period;
        _armed = true;
        _expired = false;
        _expiry = until;
        _queued = true;
    }
public:
    timer() = default;
    timer(timer&& t) noexcept : _callback(std::move(t._callback)), _expiry(std::move(t._expiry)), _period(std::move(t._period)),
            _armed(t._armed), _queued(t._queued), _expired(t._expired) {
        _link.swap_nodes(t._link);
        t._queued = false;
        t._armed = false;
    }
    explicit timer(callback_t&& callback) : _callback{std::move(callback)} {
    }
    ~timer();
    void set_callback(callback_t&& callback) {
        _callback = std::move(callback);
    }
    void arm(time_point until, compat::optional<duration> period = {});
    void rearm(time_point until, compat::optional<duration> period = {}) {
        if (_armed) {
            cancel();
        }
        arm(until, period);
    }
    void arm(duration delta) {
        return arm(Clock::now() + delta);
    }
    void arm_periodic(duration delta) {
        arm(Clock::now() + delta, {delta});
    }
    bool armed() const { return _armed; }
    bool cancel();
    time_point get_timeout() {
        return _expiry;
    }
    friend class reactor;
    friend class timer_set<timer, &timer::_link>;
};

extern template class timer<steady_clock_type>;

}

