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
 * Copyright (C) 2022 ScyllaDB.
 */

#pragma once

#include <seastar/core/abort_source.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/sleep.hh>

namespace seastar {

/// \addtogroup fiber-module
/// @{

/// Facility to tie a timeout with an abort source
/// Can be used to make abortanle fibers also support timeouts
template<typename Clock = lowres_clock>
class abort_on_expiry {
    timer<Clock> _tr;
    seastar::abort_source _as;
    optimized_optional<seastar::abort_source::subscription> _sub;
public:
    using clock = Clock;
    using time_point = typename Clock::time_point;
    /// Creates a timer and an abort source associated with it
    /// When the timer reaches timeout point it triggers an
    /// abort autimatically
    abort_on_expiry(time_point timeout) : _tr([this] {
        _as.request_abort();
    }) {
        _tr.arm(timeout);
    }
    /// Creates a timer and an abort source associated with it,
    /// chained to an external abort_source.
    /// When either the timer reaches timeout point or abort is requested
    /// via the external abort_source, it triggers an abort autimatically.
    abort_on_expiry(time_point timeout, seastar::abort_source& as) noexcept
    {
        _sub = as.subscribe([this] (const std::optional<std::exception_ptr>& opt_ex) noexcept {
            _tr.cancel();
            _as.request_abort_ex(opt_ex.value_or(_as.get_default_exception()));
        });
        if (__builtin_expect(bool(_sub), true)) {
            _tr.set_callback([this] {
                _as.request_abort_ex(std::make_exception_ptr(sleep_aborted()));
            });
            _tr.arm(timeout);
        } else {
            _as.request_abort_ex(as.get_exception());
        }
    }
    abort_on_expiry(abort_on_expiry&&) = delete;

    /// \returns abort source associated with the timeout
    seastar::abort_source& abort_source() noexcept {
        return _as;
    }
};

/// @}

}
