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
 * Copyright (C) 2016 ScyllaDB
 */

#pragma once

#include <chrono>
#include <atomic>
#include "timer.hh"

namespace seastar {

// Forward declaration.
class lowres_clock;

/// \cond internal

class lowres_clock_impl final {
public:
    typedef std::chrono::steady_clock base_clock;
    typedef base_clock::rep rep;
    // The lowres_clock's resolution is 10ms. However, to make it is easier to
    // do calcuations with std::chrono::milliseconds, we make the clock's
    // period to 1ms instead of 10ms.
    typedef std::ratio<1, 1000> period;
    typedef std::chrono::duration<rep, period> duration;
    typedef std::chrono::time_point<lowres_clock, duration> time_point;
    lowres_clock_impl();
    static time_point now() {
        auto nr = _now.load(std::memory_order_relaxed);
        return time_point(duration(nr));
    }
private:
    static void update();
    // _now is updated by cpu0 and read by other cpus. Make _now on its own
    // cache line to avoid false sharing.
    alignas(64) static std::atomic<rep> _now;
    // High resolution timer to drive this low resolution clock
    timer<> _timer;
    // High resolution timer expires every 10 milliseconds
    static constexpr std::chrono::milliseconds _granularity{10};
};

/// \endcond

//
/// \brief Low-resolution and efficient steady clock.
///
/// This is a monotonic clock with a granularity of 10 ms. Time points from this clock do not correspond to system
/// time.
///
/// The primary benefit of this clock is that invoking \c now() is inexpensive compared to
/// \c std::chrono::steady_clock::now().
///
class lowres_clock final {
public:
    using rep = lowres_clock_impl::rep;
    using period = lowres_clock_impl::period;
    using duration = lowres_clock_impl::duration;
    using time_point = lowres_clock_impl::time_point;

    static constexpr bool is_steady = true;

    ///
    /// \note Outside of a Seastar application, the result is undefined.
    ///
    static time_point now() {
        return lowres_clock_impl::now();
    }
};

}

