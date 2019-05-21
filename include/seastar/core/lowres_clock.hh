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

#include <seastar/core/cacheline.hh>
#include <seastar/core/timer.hh>

#include <cstdint>

#include <atomic>
#include <chrono>

namespace seastar {

//
// Forward declarations.
//

class lowres_clock;
class lowres_system_clock;

/// \cond internal

class lowres_clock_impl final {
public:
    using base_steady_clock = std::chrono::steady_clock;
    using base_system_clock = std::chrono::system_clock;

    // The clocks' resolutions are 10 ms. However, to make it is easier to do calculations with
    // `std::chrono::milliseconds`, we make the clock period 1 ms instead of 10 ms.
    using period = std::ratio<1, 1000>;

    using steady_rep = base_steady_clock::rep;
    using steady_duration = std::chrono::duration<steady_rep, period>;
    using steady_time_point = std::chrono::time_point<lowres_clock, steady_duration>;

    using system_rep = base_system_clock::rep;
    using system_duration = std::chrono::duration<system_rep, period>;
    using system_time_point = std::chrono::time_point<lowres_system_clock, system_duration>;

    static steady_time_point steady_now() {
        auto const nr = counters::_steady_now.load(std::memory_order_relaxed);
        return steady_time_point(steady_duration(nr));
    }

    static system_time_point system_now() {
        auto const nr = counters::_system_now.load(std::memory_order_relaxed);
        return system_time_point(system_duration(nr));
    }

    // For construction.
    friend class smp;
private:
    // Both counters are updated by cpu0 and read by other cpus. Place them on their own cache line to avoid false
    // sharing.
    struct alignas(seastar::cache_line_size) counters final {
        static std::atomic<steady_rep> _steady_now;
        static std::atomic<system_rep> _system_now;
    };

    // The timer expires every 10 ms.
    static constexpr std::chrono::milliseconds _granularity{10};

    // High-resolution timer to drive these low-resolution clocks.
    timer<> _timer{};

    static void update();

    // Private to ensure that static variables are only initialized once.
    lowres_clock_impl();
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
/// \see \c lowres_system_clock for a low-resolution clock which produces time points corresponding to system time.
///
class lowres_clock final {
public:
    using rep = lowres_clock_impl::steady_rep;
    using period = lowres_clock_impl::period;
    using duration = lowres_clock_impl::steady_duration;
    using time_point = lowres_clock_impl::steady_time_point;

    static constexpr bool is_steady = true;

    ///
    /// \note Outside of a Seastar application, the result is undefined.
    ///
    static time_point now() {
        return lowres_clock_impl::steady_now();
    }
};

///
/// \brief Low-resolution and efficient system clock.
///
/// This clock has the same granularity as \c lowres_clock, but it is not required to be monotonic and its time points
/// correspond to system time.
///
/// The primary benefit of this clock is that invoking \c now() is inexpensive compared to
/// \c std::chrono::system_clock::now().
///
class lowres_system_clock final {
public:
    using rep = lowres_clock_impl::system_rep;
    using period = lowres_clock_impl::period;
    using duration = lowres_clock_impl::system_duration;
    using time_point = lowres_clock_impl::system_time_point;

    static constexpr bool is_steady = lowres_clock_impl::base_system_clock::is_steady;

    ///
    /// \note Outside of a Seastar application, the result is undefined.
    ///
    static time_point now() {
        return lowres_clock_impl::system_now();
    }

    static std::time_t to_time_t(time_point t) {
        return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    }

    static time_point from_time_t(std::time_t t) {
        return time_point(std::chrono::duration_cast<duration>(std::chrono::seconds(t)));
    }
};

extern template class timer<lowres_clock>;

}

