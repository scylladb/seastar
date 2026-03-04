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
 * Copyright 2017 ScyllaDB
 */

#pragma once

#include <limits>
#include <cstdint>
#include <functional>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/util/critical_alloc_section.hh>

namespace seastar {
namespace memory {

///
/// Allocation failure injection framework. Allows testing for exception safety.
///
/// To exhaustively inject failure at every allocation point:
///
///     uint64_t i = 0;
///     while (true) {
///         try {
///             local_failure_injector().fail_after(i++);
///             code_under_test();
///             local_failure_injector().cancel();
///             break;
///         } catch (const std::bad_alloc&) {
///             // expected
///         }
///     }
class alloc_failure_injector {
    uint64_t _alloc_count = 0;
    uint64_t _fail_at = std::numeric_limits<uint64_t>::max();
    noncopyable_function<void()> _on_alloc_failure = [] { throw std::bad_alloc(); };
    bool _failed = false;
private:
    void fail();
public:
    /// \brief Marks a point in code which should be considered for failure injection.
    void on_alloc_point() {
        if (is_critical_alloc_section()) {
            return;
        }
        if (_alloc_count >= _fail_at) {
            fail();
        }
        ++_alloc_count;
    }

    /// Counts encountered allocation points which didn't fail and didn't have failure suppressed.
    uint64_t alloc_count() const {
        return _alloc_count;
    }

    /// Will cause count-th allocation point from now to fail, counting from 0.
    void fail_after(uint64_t count) {
        _fail_at = _alloc_count + count;
        _failed = false;
    }

    /// Cancels the failure scheduled by fail_after().
    void cancel() {
        _fail_at = std::numeric_limits<uint64_t>::max();
    }

    /// Returns true iff allocation was failed since last fail_after().
    bool failed() const {
        return _failed;
    }

    /// Runs given function with a custom failure action instead of the default std::bad_alloc throw.
    void run_with_callback(noncopyable_function<void()> callback, noncopyable_function<void()> to_run);
};

/// \cond internal
extern thread_local alloc_failure_injector the_alloc_failure_injector;
/// \endcond

/// \brief Return the shard-local \ref alloc_failure_injector instance.
inline
alloc_failure_injector& local_failure_injector() {
    return the_alloc_failure_injector;
}

#ifdef SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION

#ifdef SEASTAR_DEFAULT_ALLOCATOR
#error SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION is not supported when using SEASTAR_DEFAULT_ALLOCATOR
#endif

#endif


struct [[deprecated("Use scoped_critical_section instead")]] disable_failure_guard {
    scoped_critical_alloc_section cs;
};

/// \brief Marks a point in code which should be considered for failure injection.
inline
void on_alloc_point() {
#ifdef SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION
    local_failure_injector().on_alloc_point();
#endif
}

/// Repeatedly run func with allocation failures
///
/// Initially, allocations start to fail immediately. In each
/// subsequent run the failures start one allocation later. This
/// returns when func is run and no allocation failures are detected.
void with_allocation_failures(noncopyable_function<void()> func);

}
}
