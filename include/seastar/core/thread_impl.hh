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
#include <seastar/core/preempt.hh>
#include <seastar/util/std-compat.hh>
#include <setjmp.h>
#include <ucontext.h>
#include <atomic>
#include <chrono>

namespace seastar {
/// Clock used for scheduling threads
using thread_clock = std::chrono::steady_clock;

/// \cond internal
class thread_context;
class scheduling_group;

struct jmp_buf_link {
#ifdef SEASTAR_ASAN_ENABLED
    ucontext_t context;
    void* fake_stack = nullptr;
    const void* stack_bottom;
    size_t stack_size;
#else
    jmp_buf jmpbuf;
#endif
    jmp_buf_link* link;
    thread_context* thread;
public:
    void initial_switch_in(ucontext_t* initial_context, const void* stack_bottom, size_t stack_size);
    void switch_in();
    void switch_out();
    void initial_switch_in_completed();
    void final_switch_out();
};

extern thread_local jmp_buf_link* g_current_context;

namespace thread_impl {

inline thread_context* get() {
    return g_current_context->thread;
}

inline bool should_yield() {
    if (need_preempt()) {
        return true;
    } else {
        return false;
    }
}

void yield();
void switch_in(thread_context* to);
void switch_out(thread_context* from);
void init();

// Set while this OS thread is in the middle of a stack switch. A signal
// handler can't take a backtrace then, since the unwind information of the
// code doing the switch (e.g., glibc's swapcontext()) is wrong between loading
// the new stack pointer and the switch's end, and following it can crash. So
// the stall detector omits the backtrace while this is set.
extern constinit thread_local std::atomic_flag g_context_switch_in_progress;

// Mark the beginning and the end of a stack switch. Seastar marks its own
// seastar::thread switches; an application which switches stacks by itself,
// outside seastar::thread, should mark its switches too. Call
// begin_context_switch() right before switching stacks, and
// end_context_switch() as the first thing after arriving on the other stack,
// on the same OS thread.
inline void begin_context_switch() noexcept {
    g_context_switch_in_progress.test_and_set(std::memory_order_relaxed);
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

inline void end_context_switch() noexcept {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    g_context_switch_in_progress.clear(std::memory_order_relaxed);
}

inline bool is_context_switch_in_progress() noexcept {
    std::atomic_signal_fence(std::memory_order_seq_cst);
    return g_context_switch_in_progress.test(std::memory_order_relaxed);
}

}
}
/// \endcond


