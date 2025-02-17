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
 * Copyright 2015 Scylla DB
 */

#ifdef SEASTAR_MODULE
module;
#endif

#include <sys/mman.h>
#include <unistd.h>
#include <atomic>

#if SEASTAR_HAS_MEMBARRIER
#include <linux/membarrier.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/systemwide_memory_barrier.hh>
#include <seastar/core/cacheline.hh>
#include <seastar/util/log.hh>
#include <seastar/util/defer.hh>
#endif
#include <seastar/util/assert.hh>

namespace seastar {


#ifdef SEASTAR_HAS_MEMBARRIER

static bool has_native_membarrier = [] {
    auto r = syscall(SYS_membarrier, MEMBARRIER_CMD_QUERY, 0);
    if (r == -1) {
        return false;
    }
    int needed = MEMBARRIER_CMD_PRIVATE_EXPEDITED | MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
    if ((r & needed) != needed) {
        return false;
    }
    syscall(SYS_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0);
    return true;
}();

static bool try_native_membarrier() {
    if (has_native_membarrier) {
        syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0);
        return true;
    }
    return false;
}

#else

static bool try_native_membarrier() {
    return false;
}

#endif

// cause all threads to invoke a full memory barrier
void
systemwide_memory_barrier() {
    if (try_native_membarrier()) {
        return;
    }

    // FIXME: use sys_membarrier() when available
    static thread_local char* mem = [] {
       void* mem = mmap(nullptr, getpagesize(),
               PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS,
               -1, 0) ;
       SEASTAR_ASSERT(mem != MAP_FAILED);

       // If the user specified --lock-memory, then madvise() below will fail
       // with EINVAL, so we unlock here:
       auto r = munlock(mem, getpagesize());
       // munlock may fail on old kernels if we don't have permission. That's not
       // a problem, since if we don't have permission to unlock, we didn't have
       // permissions to lock.
       SEASTAR_ASSERT(r == 0 || errno == EPERM);

       return reinterpret_cast<char*>(mem);
    }();
    // Force page into memory to make madvise() have real work to do
    *mem = 3;
    // Evict page to force kernel to send IPI to all threads, with
    // a side effect of executing a memory barrier on those threads
    // FIXME: does this work on ARM?
    int r2 = madvise(mem, getpagesize(), MADV_DONTNEED);
    SEASTAR_ASSERT(r2 == 0);
}

struct alignas(cache_line_size) aligned_flag {
    std::atomic<bool> flag;
    bool try_lock() noexcept {
        return !flag.exchange(true, std::memory_order_relaxed);
    }
    void unlock() noexcept {
        flag.store(false, std::memory_order_relaxed);
    }
};
static aligned_flag membarrier_lock;

bool try_systemwide_memory_barrier() {
    // In 944d5fe50f3f, Linux started serializing membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED)
    // calls. This means that if all reactors enter sleep mode at the same time, they will sleep
    // on a kernel mutex while doing so. While they wait on the mutex, they cannot be woken.
    //
    // To fix this, only we serialize membarrier calls ourselves, but instead of sleeping, we just
    // return to the reactor poll loop. If an event is ready, we will wake up immediately.
    if (!membarrier_lock.try_lock()) {
        return false;
    }
    auto unlock = defer([] () noexcept {
        membarrier_lock.unlock();
    });

    if (try_native_membarrier()) {
        return true;
    }

#ifdef __aarch64__

    // Some (not all) ARM processors can broadcast TLB invalidations using the
    // TLBI instruction. On those, the mprotect trick won't work.
    static std::once_flag warn_once;
    extern logger seastar_logger;
    std::call_once(warn_once, [] {
        seastar_logger.warn("membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED) is not available, reactor will not sleep when idle. Upgrade to Linux 4.14 or later");
    });

    return false;

#endif

    systemwide_memory_barrier();
    return true;
}

}

