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
 * Copyright (C) 2023 ScyllaDB
 */

#pragma once

#include <seastar/core/circular_buffer_fixed_capacity.hh>
#include <seastar/core/internal/timers.hh>
#include <seastar/util/backtrace.hh>

#include <boost/container/static_vector.hpp>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <atomic>
#include <optional>

namespace seastar {

class reactor;

struct cpu_profiler_trace {
    using kernel_trace_vec = boost::container::static_vector<uintptr_t, 64>;
    simple_backtrace user_backtrace;
    kernel_trace_vec kernel_backtrace;
};

constexpr size_t max_number_of_traces = 128;

namespace internal {

/// A lightweight mutex designed to work with interrupts
/// utilizing only compiler barriers.
class signal_mutex {
public:
    class guard {
    private:
        signal_mutex* _mutex;
        guard(signal_mutex* m) : _mutex(m) {}
        friend class signal_mutex;
    public:
        guard(guard&& o) : _mutex(o._mutex) { o._mutex = nullptr; }
        ~guard();
    };

    // Returns a `guard` if the lock was acquired.
    // Otherwise returns a nullopt.
    std::optional<guard> try_lock();

private:
    friend class guard;
    std::atomic_bool _mutex;
};

struct cpu_profiler_config {
    bool enabled;
    std::chrono::nanoseconds period;
};

struct cpu_profiler_stats {
    unsigned dropped_samples_from_exceptions{0};
    unsigned dropped_samples_from_buffer_full{0};
    unsigned dropped_samples_from_mutex_contention{0};

    void clear_dropped() {
        dropped_samples_from_exceptions = 0;
        dropped_samples_from_buffer_full = 0;
        dropped_samples_from_mutex_contention = 0;
    }

    unsigned sum_dropped() const {
        return dropped_samples_from_buffer_full
            + dropped_samples_from_exceptions
            + dropped_samples_from_mutex_contention;
    }
};

class cpu_profiler {
private:
    circular_buffer_fixed_capacity<cpu_profiler_trace, max_number_of_traces> _traces;
    // The operations in `_traces` are not reentrant. Therefore mutex is used to ensure
    // that an interrupt cannot access `_traces` if the interrupted thread was already
    // accessing it.
    signal_mutex _traces_mutex;
    cpu_profiler_config _cfg;
    std::chrono::nanoseconds _last_set_timeout;
    cpu_profiler_stats _stats;
    bool _is_stopped{true};


    bool is_enabled() const;
    std::chrono::nanoseconds period() const;
    std::chrono::nanoseconds get_next_timeout();

protected:
    friend reactor;

public:
    static int signal_number() { return SIGRTMIN + 2; }

    cpu_profiler(cpu_profiler_config cfg) : _cfg(cfg) {}

    // Allows for the sampling period of the profiler to be adjusted
    // and the profiler to be enabled and disabled.
    void update_config(cpu_profiler_config cfg);
    // Stops the profiler if running and prevents it from starting until
    // `start()` is explicitly called.
    void stop();
    // Allows to profiler to run when it's enabled via the `cpu_profiler_config`.
    void start();
    void on_signal();
    size_t results(std::vector<cpu_profiler_trace>& results_buffer);

    virtual ~cpu_profiler() = default;
    virtual void arm_timer(std::chrono::nanoseconds) = 0;
    virtual void disarm_timer() = 0;
    virtual bool is_spurious_signal() { return false; }
    virtual std::optional<linux_perf_event::kernel_backtrace> 
    try_get_kernel_backtrace() { return std::nullopt; }
};

class cpu_profiler_posix_timer : public cpu_profiler {
    posix_timer _timer;
public:
    cpu_profiler_posix_timer(cpu_profiler_config cfg)
            : cpu_profiler(cfg) 
            // CLOCK_MONOTONIC is used here in place of CLOCK_THREAD_CPUTIME_ID.
            // This is since for intervals of ~5ms or less CLOCK_THREAD_CPUTIME_ID
            // fires 200-600% after it's configured time. Therefore it is not granular
            // enough for cases where the reactor is configured to sleep when idle and
            // is only active for short intervals. CLOCK_MONOTONIC doesn't suffer from
            // this issue.
            , _timer({signal_number()}, CLOCK_MONOTONIC) {}

    virtual ~cpu_profiler_posix_timer() override = default; 
    virtual void arm_timer(std::chrono::nanoseconds) override;
    virtual void disarm_timer() override;
};

class cpu_profiler_linux_perf_event : public cpu_profiler {
    linux_perf_event _perf_event;
public:
    static std::unique_ptr<cpu_profiler_linux_perf_event> try_make(cpu_profiler_config);
    cpu_profiler_linux_perf_event(linux_perf_event perf_event, cpu_profiler_config cfg)
            : cpu_profiler(cfg)
            , _perf_event(std::move(perf_event)) {}

    virtual ~cpu_profiler_linux_perf_event() override = default;
    virtual void arm_timer(std::chrono::nanoseconds) override;
    virtual void disarm_timer() override;
    virtual bool is_spurious_signal() override;
    virtual std::optional<linux_perf_event::kernel_backtrace> 
    try_get_kernel_backtrace() override;
};

std::unique_ptr<cpu_profiler> make_cpu_profiler(cpu_profiler_config cfg = {false, std::chrono::milliseconds(100)});

}
}
