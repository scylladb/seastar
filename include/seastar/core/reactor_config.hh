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
 * Copyright (C) 2019 ScyllaDB
 */

#pragma once

#include <seastar/util/program-options.hh>
#include <seastar/util/memory_diagnostics.hh>
#include <seastar/util/modules.hh>
#include <seastar/core/scheduling.hh>

namespace seastar {

/// \cond internal
struct reactor_config {
    sched_clock::duration task_quota;
    std::chrono::nanoseconds max_poll_time;
    bool handle_sigint = true;
    bool auto_handle_sigint_sigterm = true;
    unsigned max_networking_aio_io_control_blocks = 10000;
    bool force_io_getevents_syscall = false;
    bool kernel_page_cache = false;
    bool have_aio_fsync = false;
    unsigned max_task_backlog = 1000;
    bool strict_o_direct = true;
    bool bypass_fsync = false;
    bool no_poll_aio = false;
};
/// \endcond

class reactor_backend_selector;
class network_stack_factory;

/// Configuration for the reactor.
SEASTAR_MODULE_EXPORT
struct reactor_options : public program_options::option_group {
    /// \brief Select network stack to use.
    ///
    /// Each network stack has it corresponding
    /// \ref program_options::option_group to further tune it. The available
    /// stacks are:
    /// * Posix stack (default) - no tunable options;
    /// * Native stack  - \ref net::native_stack_options;
    program_options::selection_value<network_stack_factory> network_stack;
    /// Poll continuously (100% cpu use).
    program_options::value<> poll_mode;
    /// \brief Idle polling time in microseconds.
    ///
    /// Reduce for overprovisioned environments or laptops.
    program_options::value<unsigned> idle_poll_time_us;
    /// \brief Busy-poll for disk I/O.
    ///
    /// Reduces latency and increases throughput.
    program_options::value<bool> poll_aio;
    /// \brief Max time (ms) between polls.
    ///
    /// Default: 0.5.
    program_options::value<double> task_quota_ms;
    /// \brief Max time (ms) IO operations must take.
    ///
    /// Default: 1.5 * task_quota_ms value
    program_options::value<double> io_latency_goal_ms;
    /// \bried Dispatch rate to completion rate ratio threshold
    ///
    /// Describes the worst ratio at which seastar reactor is allowed to delay
    /// IO requests completion. If exceeded, the scheduler will consider it's
    /// disk that's the reason for completion slow-down and will scale down
    ///
    /// Default: 1.1
    program_options::value<double> io_flow_ratio_threshold;
    /// \brief If an IO request is executed longer than that, this is printed to
    /// logs with extra debugging
    ///
    /// Default: infinite (detection is OFF)
    program_options::value<unsigned> io_completion_notify_ms;
    /// \brief Maximum number of task backlog to allow.
    ///
    /// When the number of tasks grow above this, we stop polling (e.g. I/O)
    /// until it goes back below the limit.
    /// Default: 1000.
    program_options::value<unsigned> max_task_backlog;
    /// \brief Threshold in milliseconds over which the reactor is considered
    /// blocked if no progress is made.
    ///
    /// Default: 25.
    program_options::value<unsigned> blocked_reactor_notify_ms;
    /// \brief Maximum number of backtraces reported by stall detector per minute.
    ///
    /// Default: 5.
    program_options::value<unsigned> blocked_reactor_reports_per_minute;
    /// \brief Print a simplified backtrace on a single line.
    ///
    /// Default: \p true.
    program_options::value<bool> blocked_reactor_report_format_oneline;
    /// \brief Allow using buffered I/O if DMA is not available (reduces performance).
    program_options::value<> relaxed_dma;
    /// \brief Use the Linux NOWAIT AIO feature, which reduces reactor stalls due
    /// to aio (autodetected).
    program_options::value<bool> linux_aio_nowait;
    /// \brief Bypass fsync(), may result in data loss.
    ///
    /// Use for testing on consumer drives.
    /// Default: \p false.
    program_options::value<bool> unsafe_bypass_fsync;
    /// \brief Use the kernel page cache.
    ///
    /// This disables DMA (O_DIRECT). Useful for short-lived functional tests
    /// with a small data set.
    /// Default: \p false.
    program_options::value<bool> kernel_page_cache;
    /// \brief Run in an overprovisioned environment (such as docker or a laptop).
    ///
    /// Equivalent to:
    /// * \ref idle_poll_time_us = 0
    /// * \ref smp_options::thread_affinity = 0
    /// * \ref poll_aio = 0
    program_options::value<> overprovisioned;
    /// \brief Abort when seastar allocator cannot allocate memory.
    program_options::value<> abort_on_seastar_bad_alloc;
    /// \brief Force \p io_getevents(2) to issue a system call, instead of
    /// bypassing the kernel when possible.
    ///
    /// This makes strace output more useful, but slows down the application.
    /// Default: \p false.
    program_options::value<bool> force_aio_syscalls;
    /// \brief Dump diagnostics of the seastar allocator state on allocation
    /// failure.
    ///
    /// See \ref memory::alloc_failure_kind for allowed values. The diagnostics
    /// will be written to the \p seastar_memory logger, with error level.
    /// Default: \ref memory::alloc_failure_kind::critical.
    /// \note Even if the \p seastar_memory logger is set to debug or trace
    /// level, the diagnostics will be logged irrespective of this setting.
    program_options::value<memory::alloc_failure_kind> dump_memory_diagnostics_on_alloc_failure_kind;
    /// \brief Internal reactor implementation.
    ///
    /// Available backends:
    /// * \p linux-aio
    /// * \p epoll
    /// * \p io_uring
    ///
    /// Default: \p linux-aio (if available).
    program_options::selection_value<reactor_backend_selector> reactor_backend;
    /// \brief Use Linux aio for fsync() calls.
    ///
    /// This reduces latency. Requires Linux 4.18 or later.
    program_options::value<bool> aio_fsync;
    /// \brief Maximum number of I/O control blocks (IOCBs) to allocate per shard.
    ///
    /// This translates to the number of sockets supported per shard. Requires
    /// tuning \p /proc/sys/fs/aio-max-nr. Only valid for the \p linux-aio
    /// reactor backend (see \ref reactor_backend).
    ///
    /// Default: 10000.
    program_options::value<unsigned> max_networking_io_control_blocks;
    /// \brief Leave this many I/O control blocks (IOCBs) as reserve.
    ///
    /// This is to allows leaving a (small) reserve aside so other applications
    /// also using IOCBs can run alongside the seastar application.
    /// The reserve takes precedence over \ref max_networking_io_control_blocks.
    ///
    /// Default: 0
    ///
    /// \see max_networking_io_control_blocks
    program_options::value<unsigned> reserve_io_control_blocks;
    /// \brief Enable seastar heap profiling.
    ///
    /// Allocations will be sampled every N bytes on average. Zero means off.
    ///
    /// Default: 0
    ///
    /// \note Unused when seastar was compiled without heap profiling support.
    program_options::value<unsigned> heapprof;
    /// Ignore SIGINT (for gdb).
    program_options::value<> no_handle_interrupt;

    /// \cond internal
    std::string _argv0;
    bool _auto_handle_sigint_sigterm = true;
    /// \endcond

public:
    /// \cond internal
    reactor_options(program_options::option_group* parent_group);
    /// \endcond
};

}
