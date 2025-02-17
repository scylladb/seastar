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

#include <seastar/core/future.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/timer-set.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/modules.hh>
#ifndef SEASTAR_MODULE
#include <boost/intrusive/list.hpp>
#include <chrono>
#include <optional>
#endif

/// \file

/// \defgroup timers Timers
///
/// Seastar provides timers that can be defined to run a callback at a certain
/// time point in the future; timers are provided for \ref lowres_clock (10ms
/// resolution, efficient), for std::chrono::steady_clock (accurate but less
/// efficient) and for \ref manual_clock (for testing purposes).
///
/// Timers are optimized for cancellation; that is, adding a timer and cancelling
/// it is very efficient. This means that attaching a timer per object for
/// a timeout that rarely happens is reasonable; one does not have to maintain
/// a single timer and a sorted list for this use case.
///
/// Timer callbacks should be short and execute quickly. If involved processing
/// is required, a timer can launch a continuation.

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

using steady_clock_type = std::chrono::steady_clock;


/// \addtogroup timers
/// @{

/// Timer - run a callback at a certain time point in the future.
///
/// Timer callbacks should execute quickly. If more involved computation
/// is required, the timer should launch it as a fiber (or signal an
/// existing fiber to continue execution). Fibers launched from a timer
/// callback are executed under the scheduling group that was current
/// when the timer was created (see current_scheduling_group()), or the
/// scheduling that was given explicitly by the caller when the callback
/// was specified.
///
/// Expiration of a `timer<std::chrono::steady_clock>` is independent of
/// task_quota, so it has relatively high accuracy, but as a result this
/// is a relatively expensive timer. It is recommended to use `timer<lowres_clock>`
/// instead, which has very coarse resolution (~10ms) but is quite efficient.
/// It is suitable for most user timeouts.
///
/// \tparam Clock type of clock used to denote time points; can be
///        std::chrono::steady_clock_type (default), lowres_clock (more efficient
///        but with less resolution) and manual_clock_type (fine-grained control
///        for testing.
template <typename Clock = steady_clock_type>
class timer {
public:
    typedef typename Clock::time_point time_point;
    typedef typename Clock::duration duration;
    typedef Clock clock;
private:
    using callback_t = noncopyable_function<void()>;
    boost::intrusive::list_member_hook<> _link;
    scheduling_group _sg;
    callback_t _callback;
    time_point _expiry;
    std::optional<duration> _period;
    bool _armed = false;
    bool _queued = false;
    bool _expired = false;
    void readd_periodic() noexcept;
    void arm_state(time_point until, std::optional<duration> period) noexcept {
        SEASTAR_ASSERT(!_armed);
        _period = period;
        _armed = true;
        _expired = false;
        _expiry = until;
        _queued = true;
    }
public:
    /// Constructs a timer with no callback set and no expiration time.
    timer() noexcept {};  // implementation is required (instead of = default) for noexcept due to a bug in gcc 9.3.1,
                          // since boost::intrusive::list_member_hook default constructor is not specified as noexcept.
    /// Constructs a timer from another timer that is moved from.
    ///
    /// \note care should be taken when moving a timer whose callback captures `this`,
    ///       since the object pointed to by `this` may have been moved as well.
    timer(timer&& t) noexcept : _sg(t._sg), _callback(std::move(t._callback)), _expiry(std::move(t._expiry)), _period(std::move(t._period)),
            _armed(t._armed), _queued(t._queued), _expired(t._expired) {
        _link.swap_nodes(t._link);
        t._queued = false;
        t._armed = false;
    }
    /// Constructs a timer with a callback. The timer is not armed.
    ///
    /// \param sg Scheduling group to run the callback under.
    /// \param callback function (with signature `void ()`) to execute after the timer is armed and expired.
    timer(scheduling_group sg, noncopyable_function<void ()>&& callback) noexcept : _sg(sg), _callback{std::move(callback)} {
    }
    /// Constructs a timer with a callback. The timer is not armed.
    ///
    /// \param callback function (with signature `void ()`) to execute after the timer is armed and expired.
    explicit timer(noncopyable_function<void ()>&& callback) noexcept : timer(current_scheduling_group(), std::move(callback)) {
    }
    /// Destroys the timer. The timer is cancelled if armed.
    ~timer();
    /// Sets the callback function to be called when the timer expires.
    ///
    /// \param sg the scheduling group under which the callback will be executed.
    /// \param callback the callback to be executed when the timer expires.
    void set_callback(scheduling_group sg, noncopyable_function<void ()>&& callback) noexcept {
        _sg = sg;
        _callback = std::move(callback);
    }
    /// Sets the callback function to be called when the timer expires.
    ///
    /// \param callback the callback to be executed when the timer expires.
    void set_callback(noncopyable_function<void ()>&& callback) noexcept {
        set_callback(current_scheduling_group(), std::move(callback));
    }
    /// Sets the timer expiration time.
    ///
    /// It is illegal to arm a timer that has already been armed (and
    /// not disarmed by expiration or cancel()). In the current
    /// implementation, this will result in an assertion failure. See
    /// rearm().
    ///
    /// \param until the time when the timer expires
    /// \param period optional automatic rearm duration; if given the timer
    ///        will automatically rearm itself when it expires, using the period
    ///        to calculate the next expiration time.
    void arm(time_point until, std::optional<duration> period = {}) noexcept;
    /// Sets the timer expiration time. If the timer was already armed, it is
    /// canceled first.
    ///
    /// \param until the time when the timer expires
    /// \param period optional automatic rearm duration; if given the timer
    ///        will automatically rearm itself when it expires, using the period
    ///        to calculate the next expiration time.
    void rearm(time_point until, std::optional<duration> period = {}) noexcept {
        if (_armed) {
            cancel();
        }
        arm(until, period);
    }
    /// Sets the timer expiration time.
    ///
    /// It is illegal to arm a timer that has already been armed (and
    /// not disarmed by expiration or cancel()). In the current
    /// implementation, this will result in an assertion failure. See
    /// rearm().
    ///
    /// \param delta the time when the timer expires, relative to now
    void arm(duration delta) noexcept {
        return arm(Clock::now() + delta);
    }
    /// Sets the timer expiration time, with automatic rearming
    ///
    /// \param delta the time when the timer expires, relative to now. The timer
    ///        will also rearm automatically using the same delta time.
    void arm_periodic(duration delta) noexcept {
        arm(Clock::now() + delta, {delta});
    }
    /// Sets the timer expiration time, with automatic rearming.
    /// If the timer was already armed, it is canceled first.
    ///
    /// \param delta the time when the timer expires, relative to now. The timer
    ///        will also rearm automatically using the same delta time.
    void rearm_periodic(duration delta) noexcept {
        if (_armed) {
            cancel();
        }
        arm_periodic(delta);
    }
    /// Returns whether the timer is armed
    ///
    /// \return `true` if the timer is armed and has not expired yet.
    bool armed() const noexcept { return _armed; }
    /// Cancels an armed timer.
    ///
    /// If the timer was armed, it is disarmed. If the timer was not
    /// armed, does nothing.
    ///
    /// \return `true` if the timer was armed before the call.
    bool cancel() noexcept;
    /// Gets the expiration time of an armed timer.
    ///
    /// \return the time at which the timer is scheduled to expire (undefined if the
    ///       timer is not armed).
    time_point get_timeout() const noexcept {
        return _expiry;
    }

    friend class timer_set<timer, &timer::_link>;
    using set_t = timer_set<timer, &timer::_link>;
};

extern template class timer<steady_clock_type>;


/// @}
SEASTAR_MODULE_EXPORT_END
}

