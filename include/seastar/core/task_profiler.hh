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
 * Copyright (C) 2026 ScyllaDB
 */

#pragma once

#include <seastar/core/task.hh>

#include <chrono>
#include <cstdint>

namespace seastar {

template <typename CharType> class output_stream;

/// \brief Per-shard async coroutine profiler.
///
/// Tracks suspended tasks via a vector of task entries indexed by
/// task::_prof_idx. A periodic timer samples the entries, walks
/// waiting_task() chains, and accumulates per-stack counts in a
/// pmr unordered_map backed by a monotonic arena. On stop(), the
/// accumulated data is formatted and written to the given streams.
///
/// Two complementary views of the same data are emitted by stop():
///
///  - raw stream: each stack's count is the total number of leaf
///    observations across all ticks. A stack with many concurrent
///    fibers blocked at the same place accumulates a proportionally
///    larger count, even if those fibers were only blocked briefly.
///
///  - normalized stream: each stack's count is the number of distinct
///    ticks during which the stack appeared on at least one fiber.
///    This count is comparable across stacks regardless of fiber
///    multiplicity, and is a better proxy for "fraction of wall-clock
///    time this code path was blocked anywhere on the shard".
///
/// All methods operate on the current shard only. Use smp::invoke_on_all()
/// to orchestrate across shards.
class task_profiler {
public:
    struct session;

    /// Start profiling session on this shard.
    /// \param sampling_interval  Time between samples. Typical: 10-100ms.
    static void start(std::chrono::steady_clock::duration sampling_interval);

    /// Stop the profiling session, dump the accumulated samples, and
    /// reset internal state. Writes folded-stack output to two streams:
    /// \param raw_out         per-leaf sample counts (raw view)
    /// \param normalized_out  per-tick presence counts (normalized view)
    /// Both streams receive identical frame strings; only the trailing
    /// count value differs. Each stream is independently consumable by
    /// flamegraph.pl.
    static future<> stop(output_stream<char>& raw_out,
                         output_stream<char>& normalized_out);

    /// Called from set_task() when a task becomes dormant (suspended on
    /// a non-ready future). Fast path: single TLS pointer check,
    /// predicted not-taken.
    [[gnu::always_inline]]
    static void on_task_suspend(task& t) noexcept {
        if (auto* const session = _session; session != nullptr) [[unlikely]] {
            do_suspend_task(*session, t);
        }
    }

    /// Called from make_ready()/clear() when a suspended task is about
    /// to be scheduled for execution. Fast path: single field check
    /// on the task itself — no TLS access when profiler is off.
    [[gnu::always_inline]]
    static void on_task_resume(task& t) noexcept {
        if (t._prof_idx != UINT32_MAX) [[unlikely]] {
            do_resume_task(t);
        }
    }

    /// Called when a task's awaited future is already available (e.g.
    /// preemption path in co_await) — the task was never tracked, but
    /// its parent should record the completion location so it doesn't
    /// appear as a bare leaf. Fast path: single TLS pointer check.
    [[gnu::always_inline]]
    static void mark_completion(task& t) noexcept {
        if (auto* const session = _session; session != nullptr) [[unlikely]] {
            do_mark_completion(*session, t);
        }
    }

private:
    static void do_suspend_task(session& session, task& t) noexcept;
    static void do_resume_task(task& t) noexcept;
    static void do_mark_completion(session& session, task& t) noexcept;
    static void sample_once(session& session) noexcept;

    // Raw pointer (POD) rather than unique_ptr so that the TLS access on the
    // suspend/resume hot paths compiles to a single direct load (initial-exec
    // / local-exec TLS model). A non-trivially-destructible thread_local
    // would force the compiler to route accesses through a guarded init
    // wrapper, defeating that. Ownership/exception safety is handled
    // locally in start() and stop().
    inline static thread_local session* _session = nullptr;
};

} // namespace seastar
