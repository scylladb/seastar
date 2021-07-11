/*
 * Copyright (C) 2018 ScyllaDB
 */

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

#pragma once

#include <seastar/core/future.hh>
#include <seastar/util/noncopyable_function.hh>

#include <chrono>
#include <iosfwd>

// Instrumentation to detect context switches during reactor execution
// and associated stall time, intended for use in tests

namespace seastar {

namespace internal {

struct stall_report {
    uint64_t kernel_stalls;
    sched_clock::duration run_wall_time;  // excludes sleeps
    sched_clock::duration stall_time;
};

/// Run the unit-under-test (uut) function until completion, and report on any
/// reactor stalls it generated.
future<stall_report> report_reactor_stalls(noncopyable_function<future<> ()> uut);

std::ostream& operator<<(std::ostream& os, const stall_report& sr);

}

}

