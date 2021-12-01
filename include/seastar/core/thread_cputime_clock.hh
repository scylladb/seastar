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
 * Copyright (C) 2018 ScyllaDB
 */

#pragma once

#include <chrono>
#include <time.h>
#include <cassert>

namespace seastar {

class thread_cputime_clock {
public:
    using rep = int64_t;
    using period = std::chrono::nanoseconds::period;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<thread_cputime_clock, duration>;
public:
    static time_point now() {
        using namespace std::chrono_literals;

        struct timespec tp;
        [[gnu::unused]] auto ret = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tp);
        assert(ret == 0);
        return time_point(tp.tv_nsec * 1ns + tp.tv_sec * 1s);
    }
};

}

