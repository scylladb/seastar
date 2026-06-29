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
 * Copyright (C) 2019 ScyllaDB Ltd.
 */

#pragma once

#include "syscall_work_queue.hh"

#include <atomic>
#include <memory>
#include <optional>

namespace seastar {

class file_desc;

namespace internal {
// Reasons for why a function had to be submitted to the thread_pool
enum class thread_pool_submit_reason : size_t {
    // Used for aio operations what would block in `io_submit`.
    aio_fallback,
    // Used for file operations that don't have non-blocking alternatives.
    file_operation,
    // Used for process operations that don't have non-blocking alternatives.
    process_operation,
};

class submit_metrics {
    uint64_t _counters[static_cast<size_t>(thread_pool_submit_reason::process_operation) + 1]{};

public:
    void record_reason(thread_pool_submit_reason reason) {
        ++_counters[static_cast<size_t>(reason)];
    }

    uint64_t count_for(thread_pool_submit_reason reason) const {
        return _counters[static_cast<size_t>(reason)];
    }
};
} // namespace internal

class thread_pool {
    struct worker {
        syscall_work_queue inter_thread_wq;
        std::atomic<bool> stopped = { false };
        std::atomic<bool> main_thread_idle = { false };
        // Initialized right after construction (see make_worker) so the thread's
        // body can capture the owning thread_pool and this worker by reference.
        std::optional<posix_thread> thread;
    };

    file_desc& _notify_eventfd;
    internal::submit_metrics metrics;
    // Drains every blocking syscall except AIO submission fallbacks.
    std::unique_ptr<worker> _main_worker;
    // Drains blocking AIO read/write (io_submit) fallbacks. Only created when
    // the reactor uses the linux-aio backend.
    std::unique_ptr<worker> _aio_worker;
public:
    explicit thread_pool(unsigned id, file_desc& notify, bool separate_aio_queue);
    ~thread_pool();
    template <typename T, typename Func>
    future<T> submit(internal::thread_pool_submit_reason reason, Func func) noexcept {
        metrics.record_reason(reason);
        return select_worker(reason).inter_thread_wq.submit<T>(std::move(func));
    }
    uint64_t count(internal::thread_pool_submit_reason r) const { return metrics.count_for(r); }

    unsigned complete() {
        unsigned n = 0;
        for_each_worker([&] (worker& w) { n += w.inter_thread_wq.complete(); });
        return n;
    }
    // Before we enter interrupt mode, we must make sure that the syscall thread will properly
    // generate signals to wake us up. This means we need to make sure that all modifications to
    // the pending and completed fields in the inter_thread_wq are visible to all threads.
    //
    // Simple release-acquire won't do because we also need to serialize all writes that happens
    // before the syscall thread loads this value, so we'll need full seq_cst.
    void enter_interrupt_mode() {
        for_each_worker([] (worker& w) { w.main_thread_idle.store(true, std::memory_order_seq_cst); });
    }
    // When we exit interrupt mode, however, we can safely used relaxed order. If any reordering
    // takes place, we'll get an extra signal and complete will be called one extra time, which is
    // harmless.
    void exit_interrupt_mode() {
        for_each_worker([] (worker& w) { w.main_thread_idle.store(false, std::memory_order_relaxed); });
    }

private:
    template <typename Func>
    void for_each_worker(Func&& fn) {
        fn(*_main_worker);
        if (_aio_worker) {
            fn(*_aio_worker);
        }
    }
    std::unique_ptr<worker> make_worker(sstring thread_name);
    void stop_worker(worker& w) noexcept;
    worker& select_worker(internal::thread_pool_submit_reason reason) noexcept {
        if (_aio_worker && reason == internal::thread_pool_submit_reason::aio_fallback) {
            return *_aio_worker;
        }
        return *_main_worker;
    }
    void work(sstring thread_name, worker& w);
};


}
