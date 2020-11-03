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

/// \brief The kind of allocation failures to dump diagnostics report for.
///
/// Note that if the seastar_memory logger is set to level debug, there will
/// be a report dumped for any allocation failure, regardless of this
/// configuration.
enum class alloc_failure_kind {
    /// Dump diagnostic error report for none of the allocation failures.
    none,
    /// Dump diagnostic error report for critical allocation failures, see
    /// \ref scoped_critical_alloc_section.
    critical,
    /// Dump diagnostic error report for all the allocation failures.
    all,
};

/// \brief Configure when memory diagnostics are dumped.
///
/// See \ref alloc_failure_kind on available options.
/// Applies configuration on all shards.
void set_dump_memory_diagnostics_on_alloc_failure_kind(alloc_failure_kind);

/// \brief Configure when memory diagnostics are dumped.
///
/// String version. See \ref alloc_failure_kind on available options.
/// Applies configuration on all shards.
void set_dump_memory_diagnostics_on_alloc_failure_kind(std::string_view);

} // namespace memory
} // namespace seastar
