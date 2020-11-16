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

#include <seastar/util/noncopyable_function.hh>

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

/// \brief A functor which writes its argument into the diagnostics report.
using memory_diagnostics_writer = noncopyable_function<void(std::string_view)>;

/// \brief Set a producer of additional diagnostic information.
///
/// This allows the application running on top of seastar to add its own part to
/// the diagnostics dump. The application can supply higher level diagnostics
/// information, that might help explain how the memory was consumed.
///
/// The application specific part will be added just below the main stats
/// (free/used/total memory).
///
/// \param producer - the functor to produce the additional diagnostics, specific
///     to the application, to be added to the generated report. The producer is
///     passed a writer functor, which it can use to add its parts to the report.
///
/// \note As the report is generated at a time when allocations are failing, the
///     producer should try as hard as possible to not allocate while producing
///     the output.
void set_additional_diagnostics_producer(noncopyable_function<void(memory_diagnostics_writer)> producer);

/// Manually generate a diagnostics report
///
/// Note that contrary to the automated report generation (triggered by
/// allocation failure), this method does allocate memory and can fail in
/// low-memory conditions.
sstring generate_memory_diagnostics_report();

} // namespace memory
} // namespace seastar
