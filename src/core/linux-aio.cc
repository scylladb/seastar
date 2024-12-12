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
 * Copyright (C) 2017 ScyllaDB
 */

#ifdef SEASTAR_MODULE
module;
#endif

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <fmt/format.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <valgrind/valgrind.h>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/linux-aio.hh>
#include <seastar/core/print.hh>
#include <seastar/util/read_first_line.hh>
#endif

namespace seastar {

namespace internal {

namespace linux_abi {

struct linux_aio_ring {
    uint32_t id;
    uint32_t nr;
    std::atomic<uint32_t> head;
    std::atomic<uint32_t> tail;
    uint32_t magic;
    uint32_t compat_features;
    uint32_t incompat_features;
    uint32_t header_length;
};

}

using namespace linux_abi;

static linux_aio_ring* to_ring(aio_context_t io_context) {
    return reinterpret_cast<linux_aio_ring*>(uintptr_t(io_context));
}

static bool usable(const linux_aio_ring* ring) {
    return ring->magic == 0xa10a10a1 && ring->incompat_features == 0 && !RUNNING_ON_VALGRIND;
}

int io_setup(int nr_events, aio_context_t* io_context) {
    return ::syscall(SYS_io_setup, nr_events, io_context);
}

int io_destroy(aio_context_t io_context) noexcept {
   return ::syscall(SYS_io_destroy, io_context);
}

int io_submit(aio_context_t io_context, long nr, iocb** iocbs) {
    return ::syscall(SYS_io_submit, io_context, nr, iocbs);
}

int io_cancel(aio_context_t io_context, iocb* iocb, io_event* result) {
    return ::syscall(SYS_io_cancel, io_context, iocb, result);
}

static int try_reap_events(aio_context_t io_context, long min_nr, long nr, io_event* events, const ::timespec* timeout,
        bool force_syscall) {
    auto ring = to_ring(io_context);
    if (usable(ring) && !force_syscall) {
        // Try to complete in userspace, if enough available events,
        // or if the timeout is zero

        // We're the only writer to ->head, so we can load with memory_order_relaxed (assuming
        // only a single thread calls io_getevents()).
        auto head = ring->head.load(std::memory_order_relaxed);
        // The kernel will write to the ring from an interrupt and then release with a write
        // to ring->tail, so we must memory_order_acquire here.
        auto tail = ring->tail.load(std::memory_order_acquire); // kernel writes from interrupts
        auto available = tail - head;
        if (tail < head) {
            available += ring->nr;
        }
        if (available >= uint32_t(min_nr)
                || (timeout && timeout->tv_sec == 0 && timeout->tv_nsec == 0)) {
            if (!available) {
                return 0;
            }
            auto ring_events = reinterpret_cast<const io_event*>(uintptr_t(io_context) + ring->header_length);
            auto now = std::min<uint32_t>(nr, available);
            auto start = ring_events + head;
            head += now;
            if (head < ring->nr) {
                std::copy(start, start + now, events);
            } else {
                head -= ring->nr;
                auto p = std::copy(start, ring_events + ring->nr, events);
                std::copy(ring_events, ring_events + head, p);
            }
            // The kernel will read ring->head and update its view of how many entries
            // in the ring are available, so memory_order_release to make sure any ring
            // accesses are completed before the update to ring->head is visible.
            ring->head.store(head, std::memory_order_release);
            return now;
        }
    }
    return -1;
}

int io_getevents(aio_context_t io_context, long min_nr, long nr, io_event* events, const ::timespec* timeout,
        bool force_syscall) {
    auto r = try_reap_events(io_context, min_nr, nr, events, timeout, force_syscall);
    if (r >= 0) {
        return r;
    }
    return ::syscall(SYS_io_getevents, io_context, min_nr, nr, events, timeout);
}


#ifndef __NR_io_pgetevents

#  if defined(__x86_64__)
#    define __NR_io_pgetevents 333
#  elif defined(__i386__)
#    define __NR_io_pgetevents 385
#  endif

#endif

int io_pgetevents(aio_context_t io_context, long min_nr, long nr, io_event* events, const ::timespec* timeout, const sigset_t* sigmask,
        bool force_syscall) {
#ifdef __NR_io_pgetevents
    auto r = try_reap_events(io_context, min_nr, nr, events, timeout, force_syscall);
    if (r >= 0) {
        return r;
    }
    aio_sigset as;
    as.sigmask = sigmask;
    as.sigsetsize = 8;  // Can't use sizeof(*sigmask) because user and kernel sigset_t are inconsistent
    return ::syscall(__NR_io_pgetevents, io_context, min_nr, nr, events, timeout, &as);
#else
    errno = ENOSYS;
    return -1;
#endif
}

void setup_aio_context(size_t nr, linux_abi::aio_context_t* io_context) {
    auto r = io_setup(nr, io_context);
    if (r < 0) {
        char buf[1024];
#ifdef SEASTAR_STRERROR_R_CHAR_P
        const char *msg = strerror_r(errno, buf, sizeof(buf));
#else
        const char *msg = strerror_r(errno, buf, sizeof(buf)) ? "unknown error" : buf;
#endif
        if (errno == EAGAIN) {
            auto aio_max_nr = read_first_line_as<unsigned>("/proc/sys/fs/aio-max-nr");
            throw std::runtime_error(
                fmt::format("Could not setup Async I/O: {}. "
                            "The required nr_events {} exceeds the capacity in /proc/sys/fs/aio-max-nr {}. "
                            "Set /proc/sys/fs/aio-max-nr to at least {}.",
                            msg, nr, aio_max_nr, nr));
        } else {
            throw std::runtime_error(fmt::format("Could not setup Async I/O: {}", msg));
        }
    }
}

}

}
