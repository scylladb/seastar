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

#include <seastar/core/task_profiler.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/all.hh>

#include <cstdint>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>

namespace seastar {

// The profiler tracks suspended tasks via a dense vector of task entries,
// indexed by task::_prof_idx. Freed slots are removed by swapping
// with the last entry and popping, keeping the vector dense — sample_once()
// iterates only live entries with no holes to skip.
//
// Two concerns are separated:
//
// 1. Task entries (vector<task_entry>): tracks currently-suspended tasks.
//    Managed by do_suspend_task (appends entry, sets _prof_idx)
//    and do_resume_task (saves child_last_completion_loc in parent, swaps
//    out entry). Ephemeral — entries come and go as tasks suspend/resume.
//
// 2. Folded stacks map (pmr unordered_map<stack_key, stack_stats>):
//    accumulates sampling history. Populated during sample_once().
//    Persistent — survives task resume/re-suspend cycles. Released only
//    on stop() (via the arena that backs the map).
//
// On each timer tick, sample_once() first marks ancestor entries via
// waiting_task() chains (Phase 1), then iterates remaining leaves to
// build stack keys and increment map entries (Phase 2). Entries with
// child_last_completion_loc record recently-completed children whose
// entries were already freed — these appear as [completed] frames in
// the output.
//
// Two counters are accumulated per unique stack:
//
//  - leaf_samples: incremented once for every leaf observation. Sums
//    across all fibers and ticks. Stacks with high fiber multiplicity
//    (many concurrent fibers blocked at the same place) accumulate
//    proportionally larger values, which can mislead a reader trying
//    to compare wall-clock time spent in different code paths.
//
//  - tick_presence: incremented at most once per tick per stack.
//    Counts the number of ticks during which this stack appeared on
//    at least one fiber. Comparable across stacks regardless of fiber
//    multiplicity, so it is a better proxy for "fraction of wall-clock
//    time this code path was blocked anywhere on the shard".
//
// Both views are emitted by stop() to separate output streams.

namespace {

constexpr size_t max_depth = 32;

// std::source_location holds a single pointer to compiler-emitted
// metadata. A default-constructed instance has a null pointer, which
// observers expose as line()==0. Locations populated by
// __builtin_source_location() (the only producer used here, via
// std::source_location::current() and coroutine awaiter contexts)
// always have line() >= 1, so line()==0 reliably means "unset". Using
// this sentinel avoids the 16-byte overhead of std::optional<std::source_location>
// and the engaged-bit write on the suspend hot path.
inline bool is_set(const std::source_location& sl) noexcept {
    return sl.line() != 0;
}

struct stack_key {
    std::source_location frames[max_depth];
    uint8_t depth = 0;
    bool leaf_is_completed = false;

    bool operator==(const stack_key& o) const noexcept {
        return depth == o.depth
            && leaf_is_completed == o.leaf_is_completed
            && std::memcmp(frames, o.frames, depth * sizeof(std::source_location)) == 0;
    }
};

struct stack_key_hash {
    size_t operator()(const stack_key& k) const noexcept {
        size_t h = k.depth | (size_t(k.leaf_is_completed) << 8);
        for (uint8_t i = 0; i < k.depth; ++i) {
            size_t v;
            static_assert(sizeof(std::source_location) == sizeof(size_t));
            std::memcpy(&v, &k.frames[i], sizeof(v));
            // boost::hash_combine style mix
            h ^= v * 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        }
        return h;
    }
};

} // anonymous namespace

struct task_profiler::session {
    struct task_entry {
        task* t;
        // Default-constructed (null impl, line()==0) means "no recently-
        // completed child recorded". Set by mark_completion when a child
        // task resumes, so the parent leaf can show the [completed] frame.
        std::source_location child_last_completion_loc;
    };

    // Per-stack accumulators, 16 bytes each (no padding):
    //  - leaf_samples: total leaf observations across all ticks
    //  - tick_presence: number of distinct ticks in which the stack appeared
    //  - last_tick_seen: sentinel used by sample_once() to dedupe within a
    //    tick. Compared against session::tick_counter; equality means
    //    "already counted this tick".
    //
    // tick_presence and last_tick_seen are uint32_t. tick_counter wraps
    // after ~2^32 ticks; at the typical 100 ms sampling interval that's
    // ~13.6 years per session, so wraparound is not a practical concern.
    // leaf_samples stays uint64_t to avoid overflow on long sessions with
    // many concurrent fibers piling up on the same stack.
    struct stack_stats {
        uint64_t leaf_samples;
        uint32_t tick_presence;
        uint32_t last_tick_seen;
    };

    std::vector<task_entry> suspended_tasks;
    // Parallel to suspended_tasks: a packed bit vector for the
    // has_descendant flag used during sample_once(). Kept separate
    // from task_entry so Phase 1 can clear it with a single word-fill
    // and Phase 2 can read it sequentially, instead of strided
    // writes/reads through the larger task_entry struct. vector<bool>
    // packs 8 flags per byte for minimal cache footprint.
    std::vector<bool> has_descendant;
    timer<> sampling_timer;

    // Monotonic arena that backs folded_stacks. Allocations are bump-
    // pointer; deallocate() is a no-op. All memory is released at once
    // when the arena is destroyed (i.e. when the session ends). This
    // matches our access pattern: the map only grows during a session,
    // and is freed in bulk afterwards. Initial chunk is 16 KiB; the
    // resource grows geometrically when exhausted.
    std::pmr::monotonic_buffer_resource arena{16 * 1024};
    std::pmr::unordered_map<stack_key, stack_stats, stack_key_hash> folded_stacks{&arena};

    // Monotonically incremented at the start of each sample_once() call.
    // Starts at 1 so the default-initialized last_tick_seen == 0 in any
    // newly inserted stack_stats correctly indicates "never seen".
    uint32_t tick_counter = 0;
};

void task_profiler::sample_once(session& session) noexcept {
    auto& suspended = session.suspended_tasks;
    auto& has_descendant = session.has_descendant;

    // Increment first so the value is always > 0 (matches "never seen" == 0
    // sentinel in stack_stats::last_tick_seen).
    const uint32_t tick = ++session.tick_counter;

    // Phase 1: Find leaf tasks — tasks with no tracked descendant.
    // Only leaves are sampled: an ancestor's suspension time is already
    // accounted for by the leaves below it, so counting it again would
    // overcount. Walk each entry's waiting_task() chain and flag every
    // tracked ancestor as has_descendant. Early-break on already-
    // flagged ancestors — their chains were already walked, so
    // everything above is flagged too. O(N) amortised per tick.
    //
    // It would be nice to keep leaves and ancestors in separate vectors
    // so that Phase 2 only iterates leaves. But when a task resumes
    // between ticks, its parent may become a new leaf — and we can't
    // tell, because the parent-child topology is only fully known at
    // sampling time (waiting_task() links are set one at a time as
    // each coroutine in the chain suspends). So we'd still need to
    // re-classify everything each tick, gaining nothing.
    has_descendant.assign(suspended.size(), false);
    for (const auto& e : suspended) {
        for (auto* p = e.t->waiting_task(); p && p->_prof_idx != UINT32_MAX; p = p->waiting_task()) {
            if (has_descendant[p->_prof_idx]) {
                break;
            }
            has_descendant[p->_prof_idx] = true;
        }
    }

    // Phase 2: Sample leaves — entries that have no live descendant.
    stack_key key;
    for (size_t i = 0; i < suspended.size(); ++i) {
        if (has_descendant[i]) {
            continue;
        }
        const auto& e = suspended[i];

        key.depth = 0;
        key.leaf_is_completed = is_set(e.child_last_completion_loc);

        // If this leaf has a recently-completed child, include its
        // resume point as the innermost frame with [completed] annotation.
        if (key.leaf_is_completed) {
            key.frames[key.depth++] = e.child_last_completion_loc;
        }

        // Walk the task chain from leaf to root.
        for (auto* p = e.t; p && key.depth < max_depth; p = p->waiting_task()) {
            key.frames[key.depth++] = p->get_resume_point();
        }

        if (key.depth > 0) {
            auto& stats = session.folded_stacks[key];
            ++stats.leaf_samples;
            // Bump tick_presence at most once per tick per stack.
            if (stats.last_tick_seen != tick) {
                stats.last_tick_seen = tick;
                ++stats.tick_presence;
            }
        }
    }
}

void task_profiler::do_suspend_task(session& session, task& t) noexcept {
    // Append a new entry at the end of the dense vector.
    const auto idx = static_cast<uint32_t>(session.suspended_tasks.size());
    session.suspended_tasks.push_back({
        .t = &t,
    });
    t._prof_idx = idx;
}

void task_profiler::do_mark_completion(session& session, task& t) noexcept {
    auto* const parent = t.waiting_task();
    if (parent && parent->_prof_idx != UINT32_MAX) {
        auto& parent_entry = session.suspended_tasks[parent->_prof_idx];
        parent_entry.child_last_completion_loc = t.get_resume_point();
    }
}

void task_profiler::do_resume_task(task& t) noexcept {
    auto* const session = _session;
    if (!session) {
        return;
    }

    const auto idx = t._prof_idx;
    t._prof_idx = UINT32_MAX;

    // When this child task resumes, its entry is about to be removed
    // from the vector. Record the child's resume point in the parent's
    // entry so that if the parent ends up as a leaf at sampling time
    // (because no other descendant is currently tracked), the profiler
    // can still show where the most recent child was when it completed.
    do_mark_completion(*session, t);

    // Remove the entry by swapping with the last and popping.
    auto& suspended = session->suspended_tasks;
    const auto last = static_cast<uint32_t>(suspended.size() - 1);
    if (idx != last) {
        suspended[idx] = suspended[last];
        suspended[idx].t->_prof_idx = idx;
    }
    suspended.pop_back();
}

void task_profiler::start(std::chrono::steady_clock::duration sampling_interval) {
    if (_session) {
        throw std::runtime_error(format("task_profiler session already started on shard {}", this_shard_id()));
    }
    // Hold the session in unique_ptr until fully initialized (timer callback
    // set, timer armed) so that any exception from those steps frees it
    // without leaving _session pointing at a half-initialized instance.
    auto session = std::make_unique<task_profiler::session>();
    session->sampling_timer.set_callback([s = session.get()] {
        sample_once(*s);
    });
    session->sampling_timer.arm(timer<>::clock::now() + sampling_interval, {sampling_interval});
    _session = session.release();
}

future<> task_profiler::stop(output_stream<char>& raw_out,
                             output_stream<char>& normalized_out) {
    // Take ownership immediately and clear _session so on_task_suspend
    // becomes a no-op and do_resume_task handles stale indexes gracefully.
    // Doing this before any other action ensures that even if a subsequent
    // step throws, _session won't dangle and a second stop() call won't
    // double-free.
    const auto session = std::unique_ptr<task_profiler::session>(std::exchange(_session, nullptr));
    if (!session) {
        co_return;
    }

    session->sampling_timer.cancel();

    // Clear all profiler indexes on tracked tasks.
    for (const auto& e : session->suspended_tasks) {
        e.t->_prof_idx = UINT32_MAX;
    }

    // Dump folded stacks. No task pointers involved — stack_key
    // contains only source_location values (pointers to static
    // compiler metadata, always valid).
    //
    // The frame portion of the line is identical across the two output
    // streams; only the trailing count differs (leaf_samples for the raw
    // stream, tick_presence for the normalized stream). We format the
    // frame portion once and append the differing count when building
    // each line.
    //
    // The format is:
    //   <frame>;<frame>;...;<frame> <count>\n
    // where <frame> is "function_name (file:line)", or "[unknown]" when
    // function_name is empty. The leaf frame may be prefixed with
    // "[completed] " to indicate the most recent child completion point
    // for a stack whose deepest tracked task has no live descendant.
    auto format_frames = [](const stack_key& key, std::string& out) {
        out.clear();
        auto it = std::back_inserter(out);
        for (size_t i = key.depth; i > 0; --i) {
            if (i != key.depth) {
                *it++ = ';';
            }
            const bool is_completed_frame = key.leaf_is_completed && (i == 1);
            if (is_completed_frame) {
                it = fmt::format_to(it, "[completed] ");
            }
            const auto& sl = key.frames[i - 1];
            const char* const fn = sl.function_name();
            const char* const file = sl.file_name();
            const auto line = sl.line();
            if (fn && fn[0] != '\0') {
                it = fmt::format_to(it, "{}", fn);
                if (file && file[0] != '\0' && line > 0) {
                    it = fmt::format_to(it, " ({}:{})", file, line);
                }
            } else {
                it = fmt::format_to(it, "[unknown]");
            }
        }
    };

    std::string frames;
    std::string raw_line;
    std::string norm_line;
    for (const auto& [key, stats] : session->folded_stacks) {
        format_frames(key, frames);
        raw_line.clear();
        norm_line.clear();
        fmt::format_to(std::back_inserter(raw_line), "{} {}\n", frames, stats.leaf_samples);
        fmt::format_to(std::back_inserter(norm_line), "{} {}\n", frames, stats.tick_presence);
        co_await coroutine::all(
            [&] { return raw_out.write(raw_line); },
            [&] { return normalized_out.write(norm_line); });
    }
}

} // namespace seastar
