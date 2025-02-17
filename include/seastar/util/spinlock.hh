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
 * Copyright 2016 ScyllaDB
 */

#pragma once

#include <seastar/util/modules.hh>
#include <seastar/util/assert.hh>

#ifndef SEASTAR_MODULE
#include <atomic>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif
#endif

namespace seastar {

namespace internal {
#if defined(__x86_64__) || defined(__i386__)

/// \brief Puts the current CPU thread into a "relaxed" state.
///
/// This function is supposed to significantly improve the performance in situations like spinlocks when process spins
/// in a tight loop waiting for a lock. The actual implementation is different on different platforms. For more details
/// look for "Pause Intrinsic" for x86 version, and for "yield" assembly instruction documentation for Power platform.
[[gnu::always_inline]]
inline void cpu_relax() {
    _mm_pause();
}

#elif defined(__PPC__)

[[gnu::always_inline]]
inline void cpu_relax() {
    __asm__ volatile("yield");
}

#elif defined(__s390x__) || defined(__zarch__)

// FIXME: there must be a better way
[[gnu::always_inline]]
inline void cpu_relax() {}

#elif defined(__aarch64__)

[[gnu::always_inline]]
inline void cpu_relax() {
    __asm__ volatile("yield");
}

#else

[[gnu::always_inline]]
inline void cpu_relax() {}
#warning "Using an empty cpu_relax() for this architecture"

#endif


}

namespace util {

// Spin lock implementation.
// BasicLockable.
// Async-signal safe.
// unlock() "synchronizes with" lock().
SEASTAR_MODULE_EXPORT
class spinlock {
    std::atomic<bool> _busy = { false };
public:
    spinlock() = default;
    spinlock(const spinlock&) = delete;
    ~spinlock() { SEASTAR_ASSERT(!_busy.load(std::memory_order_relaxed)); }
    bool try_lock() noexcept {
        return !_busy.exchange(true, std::memory_order_acquire);
    }
    void lock() noexcept {
        while (_busy.exchange(true, std::memory_order_acquire)) {
            while (_busy.load(std::memory_order_relaxed)) {
                internal::cpu_relax();
            }
        }
    }
    void unlock() noexcept {
        _busy.store(false, std::memory_order_release);
    }
};

}

}
