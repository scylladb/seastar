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
 * Copyright 2020 ScyllaDB
 */

#pragma once

namespace seastar {
namespace memory {

#ifdef SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION

/// \cond internal
namespace internal {

// This variable is used in hot paths so we want to avoid the compiler
// generating TLS init guards for it. In C++20 we have constinit to tell the
// compiler that it can be initialized compile time (although gcc still doesn't
// completely drops the init guards - https://gcc.gnu.org/bugzilla/show_bug.cgi?id=97848).
// In < c++20 we use `__thread` which results in no TLS init guards generated.
#ifdef __cpp_constinit
extern thread_local constinit volatile int critical_alloc_section;
#else
extern __thread volatile int critical_alloc_section;
#endif

} // namespace internal
/// \endcond

/// \brief Marks scopes that contain critical allocations.
///
/// Critical allocations are those, whose failure the application cannot
/// tolerate. In a perfect world, there should be no such allocation, but we
/// don't live in a perfect world.
/// This information is used by other parts of the memory subsystem:
/// * \ref alloc_failure_injector will not inject errors into these scopes.
/// * A memory diagnostics report will be dumped if an allocation fails in these
///   scopes when the memory diagnostics subsystem is configured to dump reports
///   for \ref alloc_failure_kind \ref alloc_failure_kind::critical or above.
///   See \ref set_dump_memory_diagnostics_on_alloc_failure_kind().
class scoped_critical_alloc_section {
public:
    scoped_critical_alloc_section() {
        // we assume the critical_alloc_section is thread local
        // and there's seastar threads are non-preemptive.
        // Otherwise, this would require an atomic variable
        internal::critical_alloc_section = internal::critical_alloc_section + 1;
    }
    ~scoped_critical_alloc_section() {
        internal::critical_alloc_section = internal::critical_alloc_section - 1;
    }
};

/// \brief Is the current context inside a critical alloc section?
///
/// Will return true if there is at least one \ref scoped_critical_alloc_section
/// alive in the current scope or the scope of any of the caller functions.
inline bool is_critical_alloc_section() {
    return bool(internal::critical_alloc_section);
}

#else   // SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION

struct [[maybe_unused]] scoped_critical_alloc_section {};

inline bool is_critical_alloc_section() {
    return false;
}

#endif  // SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION

} // namespace seastar
} // namespace memory
