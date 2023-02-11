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

/// \endcond

//
/// \brief Low-resolution and efficient steady clock.
///
/// This is a monotonic clock with a granularity of ~task_quota. Time points from this clock do not correspond to system
/// time.
///
/// The primary benefit of this clock is that invoking \c now() is inexpensive compared to
/// \c std::chrono::steady_clock::now().
///
/// \see \c lowres_system_clock for a low-resolution clock which produces time points corresponding to system time.
///
class lowres_clock final {
public:
    using rep = std::chrono::steady_clock::rep;
    using period = std::chrono::steady_clock::period;
    using duration = std::chrono::steady_clock::duration;
    using time_point = std::chrono::time_point<lowres_clock, duration>;
    static constexpr bool is_steady = true;
private:
#ifdef SEASTAR_BUILD_SHARED_LIBS
    static thread_local time_point _now;
#else
    // Use inline variable to prevent the compiler from introducing an initialization guard
    inline static thread_local time_point _now;
#endif
public:
    ///
    /// \note Outside of a Seastar application, the result is undefined.
    ///
    static time_point now() noexcept {
        return _now;
    }

    static void update() noexcept;
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
    using rep = std::chrono::system_clock::rep;
    using period = std::chrono::system_clock::period;
    using duration = std::chrono::system_clock::duration;
    using time_point = std::chrono::time_point<lowres_system_clock, duration>;
    static constexpr bool is_steady = false;
private:
#ifdef SEASTAR_BUILD_SHARED_LIBS
    static thread_local time_point _now;
#else
    // Use inline variable to prevent the compiler from introducing an initialization guard
    inline static thread_local time_point _now;
#endif
    friend class lowres_clock; // for updates
public:
    ///
    /// \note Outside of a Seastar application, the result is undefined.
    ///
    static time_point now() noexcept {
        return _now;
    }

    static std::time_t to_time_t(time_point t) noexcept {
        return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
    }

    static time_point from_time_t(std::time_t t) noexcept {
        return time_point(std::chrono::duration_cast<duration>(std::chrono::seconds(t)));
    }
};

extern template class timer<lowres_clock>;

}

