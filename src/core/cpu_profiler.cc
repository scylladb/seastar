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

#include <chrono>
#include <optional>
#include <random>

#include <seastar/core/internal/cpu_profiler.hh>
#include <seastar/util/log.hh>

namespace seastar {
seastar::logger cpu_profiler_logger("cpu_profiler");

namespace internal {

using namespace std::chrono_literals;

signal_mutex::guard::~guard() {
    if (_mutex == nullptr) {
        return;
    }
    // Ensure the subsequent store isn't hoisted by the the
    // compiler into the critical section it's intended to 
    // protect.
    std::atomic_signal_fence(std::memory_order_release);
    _mutex->_mutex.store(false, std::memory_order_relaxed);
}

std::optional<signal_mutex::guard> signal_mutex::try_lock() {
    if (!_mutex.load(std::memory_order_relaxed)) {
        _mutex.store(true, std::memory_order_relaxed);
        // Ensure that this read-modify-update operation isn't
        // mixed into the critical section it's intended to protect
        // by the compiler.
        std::atomic_signal_fence(std::memory_order_acq_rel);
        return {guard(this)};
    } 

    return std::nullopt;
}

/**
 * The profiler breaks sample periods into windows of size _cfg.period.
 * I.e, [1ns, _cfg.period), [_cfg.period, 2*_cfg.period)... etc. And it
 * will ensure that a sample is taken exactly once per window. The goal
 * of this function is to randomly select a point in the window to sample.
 * This avoids potential bias from sampling at the same point in time every
 * window.
 */
std::chrono::nanoseconds cpu_profiler::get_next_timeout() {
    using ns_rep = std::chrono::nanoseconds::rep;
    static thread_local std::mt19937_64 gen = std::mt19937_64(std::default_random_engine()());

    auto remaining_time_for_last_profiler_period = _cfg.period - _last_set_timeout;

    std::uniform_int_distribution<ns_rep> profiler_dist{0, _cfg.period / 1ns};
    auto rwait = profiler_dist(gen);
    _last_set_timeout = std::chrono::nanoseconds(rwait);

    // The interrupt needs to be at least 1ns for perf_event
    auto wait = std::max(_last_set_timeout + remaining_time_for_last_profiler_period, 1ns);
    return wait;
}

bool cpu_profiler::is_enabled() const {
    return _cfg.enabled;
}

std::chrono::nanoseconds cpu_profiler::period() const {
    return _cfg.period;
}

void cpu_profiler::update_config(cpu_profiler_config cfg) {
    auto is_stopped = _is_stopped;
    stop();
    _cfg = cfg;
    // Don't start the profiler if it's been explicitly 
    // stopped elsewhere.
    if (!is_stopped) {
        start();
    }
}

void cpu_profiler::stop() {
    if (_is_stopped) {
        return;
    }
    if (_cfg.enabled) {
        disarm_timer();
    }
    _is_stopped = true;
}

void cpu_profiler::start() {
    _is_stopped = false;
    if (_cfg.enabled) {
        _last_set_timeout = _cfg.period;
        auto next = get_next_timeout();
        arm_timer(next);
    }
}

void cpu_profiler::on_signal() {
    if (is_spurious_signal()) {
        return;
    }

    // Skip the sample if the main thread is currently reading
    // _traces. This case shouldn't happen often though.
    if (auto guard_opt = _traces_mutex.try_lock(); guard_opt.has_value()) {
        // The oldest trace will be overridden if the circular
        // buffer is full so update the bookkeeping to indicate
        // this.
        if (_traces.size() == _traces.capacity()) {
            _traces.pop_front();
            _dropped_samples++;
        }
        _traces.emplace_back();
        _traces.back().user_backtrace = current_backtrace_tasklocal();

        auto kernel_bt = try_get_kernel_backtrace();
        if (kernel_bt) {
            auto& kernel_vec = _traces.back().kernel_backtrace;

            kernel_bt->read_backtrace([&] (uintptr_t addr) {
                if((kernel_vec.size() + 1) <= kernel_vec.max_size()) {
                    kernel_vec.push_back(addr);
                }
            });
        }
    }

    auto next = get_next_timeout();
    arm_timer(next);
}
 
size_t cpu_profiler::results(std::vector<cpu_profiler_trace>& results_buffer) {
    // Since is this not called in the interrupt it should always succeed
    // in acquiring the lock.
    auto guard_opt = _traces_mutex.try_lock();
    if (!guard_opt.has_value()) {
        results_buffer.clear();
        return 0;
    }

    results_buffer.assign(_traces.cbegin(), _traces.cend());
    _traces.clear();
    
    return std::exchange(_dropped_samples, 0);
}

void cpu_profiler_posix_timer::arm_timer(std::chrono::nanoseconds ns) {
    return _timer.arm_timer(ns);
}

void cpu_profiler_posix_timer::disarm_timer() {
    return _timer.disarm_timer();
}

bool cpu_profiler_linux_perf_event::is_spurious_signal() {
    return _perf_event.is_spurious_signal();
}

std::optional<linux_perf_event::kernel_backtrace> 
cpu_profiler_linux_perf_event::try_get_kernel_backtrace() {
    return _perf_event.try_get_kernel_backtrace();
}

void cpu_profiler_linux_perf_event::arm_timer(std::chrono::nanoseconds ns) {
    _perf_event.arm_timer(ns);
}

void cpu_profiler_linux_perf_event::disarm_timer() {
    _perf_event.disarm_timer();
}

std::unique_ptr<cpu_profiler_linux_perf_event>
cpu_profiler_linux_perf_event::try_make(cpu_profiler_config cfg) {
    return std::make_unique<cpu_profiler_linux_perf_event>(
            linux_perf_event::try_make({signal_number()}), std::move(cfg));
}

std::unique_ptr<cpu_profiler> make_cpu_profiler(cpu_profiler_config cfg) {
    std::unique_ptr<cpu_profiler> profiler;

    try {
        profiler = cpu_profiler_linux_perf_event::try_make(cfg);

    } catch (std::system_error& e) {
        // This failure occurs when /proc/sys/kernel/perf_event_paranoid is set
        // to 2 or higher, and is expected since most distributions set it to that
        // way as of 2023. In this case we log a different message and only at INFO
        // level on shard 0.
        if (e.code() == std::error_code(EACCES, std::system_category())) {
            cpu_profiler_logger.info0("Perf-based cpu profiler creation failed (EACCESS), "
                    "try setting /proc/sys/kernel/perf_event_paranoid to 1 or less to "
                    "enable kernel backtraces: falling back to posix timer.");
        } else {
            cpu_profiler_logger.warn("Creation of perf_event based cpu profiler failed: falling back to posix timer: {}", e.what());
        }
    } catch (...) {
        cpu_profiler_logger.warn("Creation of perf_event based cpu profiler failed: falling back to posix timer: {}", std::current_exception());
    }

    if (!profiler) {
        profiler = std::make_unique<cpu_profiler_posix_timer>(cfg);
    }

    return profiler;
}

} // namespace internal
} // namespace seastar
