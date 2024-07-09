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
 * Copyright (C) 2016 ScyllaDB.
 */

#pragma once
#ifndef SEASTAR_MODULE
#include <atomic>
#include <seastar/util/modules.hh>
#endif

namespace seastar {

namespace internal {

struct preemption_monitor {
    // We preempt when head != tail
    // This happens to match the Linux aio completion ring, so we can have the
    // kernel preempt a task by queuing a completion event to an io_context.
    std::atomic<uint32_t> head;
    std::atomic<uint32_t> tail;
};

#ifdef SEASTAR_BUILD_SHARED_LIBS
const preemption_monitor*& get_need_preempt_var();
#else
inline const preemption_monitor*& get_need_preempt_var() {
    static preemption_monitor bootstrap_preemption_monitor;
    static thread_local const preemption_monitor* g_need_preempt = &bootstrap_preemption_monitor;
    return g_need_preempt;
}
#endif

void set_need_preempt_var(const preemption_monitor* pm);

inline
bool
monitor_need_preempt() noexcept {
    // prevent compiler from eliminating loads in a loop
    std::atomic_signal_fence(std::memory_order_seq_cst);
    auto np = internal::get_need_preempt_var();
    // We aren't reading anything from the ring, so we don't need
    // any barriers.
    auto head = np->head.load(std::memory_order_relaxed);
    auto tail = np->tail.load(std::memory_order_relaxed);
    // Possible optimization: read head and tail in a single 64-bit load,
    // and find a funky way to compare the two 32-bit halves.
    return __builtin_expect(head != tail, false);
}

}

SEASTAR_MODULE_EXPORT
inline bool need_preempt() noexcept {
#ifndef SEASTAR_DEBUG
    return internal::monitor_need_preempt();
#else
    return true;
#endif
}

namespace internal {



// Same as need_preempt(), but for the scheduler's use. Outside debug
// mode they have the same meaning - the task quota expired and we need
// to check for I/O.
inline
bool
scheduler_need_preempt() {
#ifndef SEASTAR_DEBUG
    return need_preempt();
#else
    // Within the scheduler, preempting all the time (as need_preempt()
    // does in debug mode) reduces performance drastically since we check
    // for I/O after every task. Since we don't care about latency in debug
    // mode, run some random-but-bounded number of tasks instead. Latency
    // will be high if those tasks are slow, but this is debug mode anyway.
    // We still check if preemption was requested to allow lowres_clock
    // updates.
    static thread_local unsigned counter = 0;
    return ++counter % 64 == 0 || monitor_need_preempt();;
#endif
}

}

}
