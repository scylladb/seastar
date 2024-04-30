
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

#ifndef SEASTAR_MODULE
#include <signal.h>
#include <atomic>
#include <limits>
#include <chrono>
#include <functional>
#include <memory>
#include <linux/perf_event.h>
#endif
#include <seastar/core/posix.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/internal/timers.hh>
#include <seastar/util/modules.hh>

namespace seastar {

class reactor;
class thread_cputime_clock;

namespace internal {

struct cpu_stall_detector_config {
    std::chrono::duration<double> threshold = std::chrono::seconds(2);
    unsigned stall_detector_reports_per_minute = 1;
    float slack = 0.3;  // fraction of threshold that we're allowed to overshoot
    bool oneline = true; // print a simplified backtrace on a single line
    std::function<void ()> report;  // alternative reporting function for tests
};

// Detects stalls in continuations that run for too long
class cpu_stall_detector {
protected:
    std::atomic<uint64_t> _last_tasks_processed_seen{};
    unsigned _stall_detector_reports_per_minute;
    std::atomic<uint64_t> _stall_detector_missed_ticks = { 0 };
    unsigned _reported = 0;
    unsigned _total_reported = 0;
    unsigned _max_reports_per_minute;
    unsigned _shard_id;
    unsigned _thread_id;
    unsigned _report_at{};
    sched_clock::time_point _minute_mark{};
    sched_clock::time_point _rearm_timer_at{};
    sched_clock::time_point _run_started_at{};
    sched_clock::duration _threshold;
    sched_clock::duration _slack;
    cpu_stall_detector_config _config;
    seastar::metrics::metric_groups _metrics;
    friend reactor;
    virtual bool is_spurious_signal() {
        return false;
    }
    virtual void maybe_report_kernel_trace() {}
private:
    void maybe_report();
    virtual void arm_timer() = 0;
    void report_suppressions(sched_clock::time_point now);
    void reset_suppression_state(sched_clock::time_point now);
public:
    using clock_type = thread_cputime_clock;
public:
    explicit cpu_stall_detector(cpu_stall_detector_config cfg = {});
    virtual ~cpu_stall_detector() = default;
    static int signal_number() { return SIGRTMIN + 1; }
    void start_task_run(sched_clock::time_point now);
    void end_task_run(sched_clock::time_point now);
    void generate_trace();
    void update_config(cpu_stall_detector_config cfg);
    cpu_stall_detector_config get_config() const;
    void on_signal();
    virtual void start_sleep() = 0;
    void end_sleep();
};

class cpu_stall_detector_posix_timer : public cpu_stall_detector {
    posix_timer _timer;
public:
    explicit cpu_stall_detector_posix_timer(cpu_stall_detector_config cfg = {});
    virtual ~cpu_stall_detector_posix_timer() override = default;
private:
    virtual void arm_timer() override;
    virtual void start_sleep() override;
};

class cpu_stall_detector_linux_perf_event : public cpu_stall_detector {
    linux_perf_event _perf_event;
public:
    static std::unique_ptr<cpu_stall_detector_linux_perf_event> try_make(cpu_stall_detector_config cfg = {});
    explicit cpu_stall_detector_linux_perf_event(linux_perf_event perf_event, cpu_stall_detector_config cfg = {});
    virtual ~cpu_stall_detector_linux_perf_event() override = default;
    virtual void arm_timer() override;
    virtual void start_sleep() override;
    virtual bool is_spurious_signal() override;
    virtual void maybe_report_kernel_trace() override;
};

std::unique_ptr<cpu_stall_detector> make_cpu_stall_detector(cpu_stall_detector_config cfg = {});

}
}
