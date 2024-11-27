
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
    timer_t _timer;
public:
    explicit cpu_stall_detector_posix_timer(cpu_stall_detector_config cfg = {});
    virtual ~cpu_stall_detector_posix_timer() override;
private:
    virtual void arm_timer() override;
    virtual void start_sleep() override;
};

class cpu_stall_detector_linux_perf_event : public cpu_stall_detector {
    file_desc _fd;
    bool _enabled = false;
    uint64_t _current_period = 0;
    struct ::perf_event_mmap_page* _mmap;
    char* _data_area;
    size_t _data_area_mask;
    // after the detector has been armed (i.e., _enabled is true), this
    // is the moment at or after which the next signal is expected to occur
    // and can be used for detecting spurious signals
    sched_clock::time_point _next_signal_time{};
private:
    class data_area_reader {
        cpu_stall_detector_linux_perf_event& _p;
        const char* _data_area;
        size_t _data_area_mask;
        uint64_t _head;
        uint64_t _tail;
    public:
        explicit data_area_reader(cpu_stall_detector_linux_perf_event& p)
                : _p(p)
                , _data_area(p._data_area)
                , _data_area_mask(p._data_area_mask) {
            _head = _p._mmap->data_head;
            _tail = _p._mmap->data_tail;
            std::atomic_thread_fence(std::memory_order_acquire); // required after reading data_head
        }
        ~data_area_reader() {
            std::atomic_thread_fence(std::memory_order_release); // not documented, but probably required before writing data_tail
            _p._mmap->data_tail = _tail;
        }
        uint64_t read_u64() {
            uint64_t ret;
            // We cannot wrap around if the 8-byte unit is aligned
            std::copy_n(_data_area + (_tail & _data_area_mask), 8, reinterpret_cast<char*>(&ret));
            _tail += 8;
            return ret;
        }
        template <typename S>
        S read_struct() {
            static_assert(sizeof(S) % 8 == 0);
            S ret;
            char* p = reinterpret_cast<char*>(&ret);
            for (size_t i = 0; i != sizeof(S); i += 8) {
                uint64_t w = read_u64();
                std::copy_n(reinterpret_cast<const char*>(&w), 8, p + i);
            }
            return ret;
        }
        void skip(uint64_t bytes_to_skip) {
            _tail += bytes_to_skip;
        }
        // skip all the remaining data in the buffer, as-if calling read until
        // have_data returns false (but much faster)
        void skip_all() {
            _tail = _head;
        }
        bool have_data() const {
            return _head != _tail;
        }
    };
public:
    static std::unique_ptr<cpu_stall_detector_linux_perf_event> try_make(cpu_stall_detector_config cfg = {});
    explicit cpu_stall_detector_linux_perf_event(file_desc fd, cpu_stall_detector_config cfg = {});
    ~cpu_stall_detector_linux_perf_event();
    virtual void arm_timer() override;
    virtual void start_sleep() override;
    virtual bool is_spurious_signal() override;
    virtual void maybe_report_kernel_trace() override;
};

std::unique_ptr<cpu_stall_detector> make_cpu_stall_detector(cpu_stall_detector_config cfg = {});

}
}
