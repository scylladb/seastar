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


#ifdef SEASTAR_MODULE
module;
#endif

#include <atomic>
#include <cstdint>
#include <array>
#include <pthread.h>
#include <signal.h>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/reactor.hh>
#include "core/thread_pool.hh"
#endif
#include <seastar/util/assert.hh>

namespace seastar {

thread_pool::thread_pool(reactor& r, sstring name) : _reactor(r), _worker_thread([this, name] { work(name); }) {
}

void thread_pool::work(sstring name) {
    pthread_setname_np(pthread_self(), name.c_str());
    sigset_t mask;
    sigfillset(&mask);
    auto r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
    throw_pthread_error(r);
    std::array<syscall_work_queue::work_item*, syscall_work_queue::queue_length> tmp_buf;
    while (true) {
        uint64_t count;
        auto r = ::read(inter_thread_wq._start_eventfd.get_read_fd(), &count, sizeof(count));
        SEASTAR_ASSERT(r == sizeof(count));
        if (_stopped.load(std::memory_order_relaxed)) {
            break;
        }
        auto end = tmp_buf.data();
        inter_thread_wq._pending.consume_all([&] (syscall_work_queue::work_item* wi) {
            *end++ = wi;
        });
        for (auto p = tmp_buf.data(); p != end; ++p) {
            auto wi = *p;
            wi->process();
            inter_thread_wq._completed.push(wi);

            // Prevent the following load of _main_thread_idle to be hoisted before the writes to _completed above.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (_main_thread_idle.load(std::memory_order_relaxed)) {
                uint64_t one = 1;
                auto res = ::write(_reactor._notify_eventfd.get(), &one, 8);
                SEASTAR_ASSERT(res == 8 && "write(2) failed on _reactor._notify_eventfd");
            }
        }
    }
}

thread_pool::~thread_pool() {
    _stopped.store(true, std::memory_order_relaxed);
    inter_thread_wq._start_eventfd.signal(1);
    _worker_thread.join();
}

}
