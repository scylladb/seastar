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
 * Copyright 2022 ScyllaDB
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <seastar/util/program-options.hh>
#include <seastar/util/modules.hh>
#include <string>
#endif

/// \file

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

enum class memory_allocator {
    /// Seastar's own allocator, optimized for its shard-per core design.
    /// Strongly recommended for most seastar apps.
    seastar,
    /// The standard allocator from libc.
    /// Useful when writing seastar-based tool apps that want to
    /// minimize their footprint on the system they run on.
    standard,
};

/// Configuration for the multicore aspect of seastar.
struct smp_options : public program_options::option_group {
    /// Number of threads (default: one per CPU).
    program_options::value<unsigned> smp;
    /// CPUs to use (in cpuset(7) format; default: all)).
    program_options::value<resource::cpuset> cpuset;
    /// Memory to use, in bytes (ex: 4G) (default: all).
    program_options::value<std::string> memory;
    /// Memory reserved to the OS and other processes (if \ref memory not specified),
    /// and is not used by seastar.
    program_options::value<std::string> reserve_memory;
    /// Path to accessible hugetlbfs mount (typically /dev/hugepages/something).
    program_options::value<std::string> hugepages;
    /// Lock all memory (prevents swapping).
    program_options::value<bool> lock_memory;
    /// Pin threads to their cpus (disable for overprovisioning).
    ///
    /// Default: \p true.
    program_options::value<bool> thread_affinity;
    /// \brief Number of IO groups.
    ///
    /// Each IO group will be responsible for a fraction of the IO requests.
    /// Defaults to the number of NUMA nodes
    /// \note Unused when seastar is compiled without \p HWLOC support.
    program_options::value<unsigned> num_io_groups;
    /// Path to a YAML file describing the characteristics of the I/O Subsystem.
    program_options::value<std::string> io_properties_file;
    /// A YAML string describing the characteristics of the I/O Subsystem.
    program_options::value<std::string> io_properties;
    /// Enable mbind.
    ///
    /// Default: \p true.
    program_options::value<bool> mbind;
    /// Enable workaround for glibc/gcc c++ exception scalablity problem.
    ///
    /// Default: \p true.
    /// \note Unused when seastar is compiled without the exception scaling support.
    program_options::value<bool> enable_glibc_exception_scaling_workaround;
    /// If some CPUs are found not to have any local NUMA nodes, allow assigning
    /// them to remote ones.
    /// \note Unused when seastar is compiled without \p HWLOC support.
    program_options::value<bool> allow_cpus_in_remote_numa_nodes;

    /// Memory allocator to use.
    ///
    /// The following options only have effect if the \ref memory_allocator::seastar is used:
    /// * \ref smp_options::memory
    /// * \ref smp_options::reserve_memory
    /// * \ref smp_options::hugepages
    /// * \ref smp_options::mbind
    /// * \ref reactor_options::heapprof
    /// * \ref reactor_options::abort_on_seastar_bad_alloc
    /// * \ref reactor_options::dump_memory_diagnostics_on_alloc_failure_kind
    /// \note Unused when seastar was compiled without the custom allocator support.
    /// The options above won't be applied in this case either.
    seastar::memory_allocator memory_allocator = memory_allocator::seastar;

    /// \cond internal
    /// Additional memory reserved to the OS and other processes for each shard (added to the default value or the value specified by \ref reserve_memory),
    /// and is not used by seastar.
    size_t reserve_additional_memory_per_shard = 0;
    /// \endcond
public:
    smp_options(program_options::option_group* parent_group);
};

SEASTAR_MODULE_EXPORT_END
}
