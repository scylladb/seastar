
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

#include <signal.h>
#include <limits>
#include <chrono>

namespace seastar {

class reactor;

namespace internal {

struct cpu_stall_detector_config {
    std::chrono::duration<double> threshold = std::chrono::seconds(2);
    unsigned stall_detector_reports_per_minute = 1;
};

// Detects stalls in continuations that run for too long
class cpu_stall_detector {
    reactor* _r;
    std::atomic<unsigned> _tasks_processed_stalled = { 0 };
    unsigned _tasks_processed_report_threshold;
    unsigned _stall_detector_reports_per_minute;
    std::atomic<uint64_t> _stall_detector_missed_ticks = { 0 };
    unsigned _reported = 0;
    unsigned _ticks = 0;
    unsigned _ticks_per_minute;
    unsigned _max_reports_per_minute;
    unsigned _shard_id;
    unsigned _thread_id;
    unsigned _report_at{};
    unsigned _saved_missed_ticks{};
    uint64_t _last_tasks_processed_seen{};
    uint64_t _last_polls_seen{};
    cpu_stall_detector_config _config;
    friend reactor;
private:
    void maybe_report(pthread_t who, int sig);
public:
    cpu_stall_detector(reactor* r, cpu_stall_detector_config cfg = {});
    static int signal_number() { return SIGRTMIN + 1; }
    void generate_trace();
    void update_config(cpu_stall_detector_config cfg);
    cpu_stall_detector_config get_config() const;
    void tick();
    void account_for_missed_ticks(std::chrono::steady_clock::duration idle_time);
};

}
}
