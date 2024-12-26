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
#include <seastar/core/timed_out_error.hh>
#include <seastar/util/modules.hh>

namespace seastar {

/// \addtogroup fiber-module
/// @{

/// Facility to tie a timeout with an abort source
/// Can be used to make abortable fibers also support timeouts
SEASTAR_MODULE_EXPORT
template<typename Clock = lowres_clock>
class abort_on_expiry {
    timer<Clock> _tr;
    seastar::abort_source _as;
public:
    using clock = Clock;
    using time_point = typename Clock::time_point;
    /// Creates a timer and an abort source associated with it
    /// When the timer reaches timeout point it triggers an
    /// abort automatically
    abort_on_expiry(time_point timeout) : _tr([this] {
        _as.request_abort_ex(timed_out_error{});
    }) {
        _tr.arm(timeout);
    }
    abort_on_expiry(abort_on_expiry&&) = delete;

    /// \returns abort source associated with the timeout
    seastar::abort_source& abort_source() {
        return _as;
    }
};

/// @}

}
