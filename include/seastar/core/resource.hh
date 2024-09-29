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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#pragma once

#include <seastar/util/spinlock.hh>
#include <seastar/util/modules.hh>
#ifndef SEASTAR_MODULE
#include <cassert>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <set>
#include <sched.h>
#include <unordered_map>
#ifdef SEASTAR_HAVE_HWLOC
#include <hwloc.h>
#endif
#endif

namespace seastar {

cpu_set_t cpuid_to_cpuset(unsigned cpuid);
class io_queue;
class io_group;

namespace resource {

using std::optional;

using cpuset = std::set<unsigned>;

/// \cond internal
std::optional<cpuset> parse_cpuset(std::string value);
/// \endcond

namespace hwloc::internal {

#ifdef SEASTAR_HAVE_HWLOC
class topology_holder {
    hwloc_topology_t _topology;

public:
    topology_holder() noexcept
        : _topology(nullptr)
    { }

    topology_holder(topology_holder&& o) noexcept;

    ~topology_holder();

    topology_holder& operator=(topology_holder&& o) noexcept;

    operator bool() const noexcept {
        return _topology != nullptr;
    }

    void init_and_load();
    hwloc_topology_t get();
};

#else // SEASTAR_HAVE_HWLOC

struct topology_holder {};

#endif // SEASTAR_HAVE_HWLOC

} // namespace hwloc::internal

SEASTAR_MODULE_EXPORT_BEGIN

struct configuration {
    optional<size_t> total_memory;
    optional<size_t> reserve_memory;  // if total_memory not specified
    size_t reserve_additional_memory_per_shard;
    size_t cpus;
    cpuset cpu_set;
    bool assign_orphan_cpus = false;
    std::vector<dev_t> devices;
    unsigned num_io_groups;
    hwloc::internal::topology_holder topology;
};

struct memory {
    size_t bytes;
    unsigned nodeid;

};

struct io_queue_topology {
    std::vector<std::unique_ptr<io_queue>> queues;
    std::vector<unsigned> shard_to_group;
    std::vector<unsigned> shards_in_group;
    std::vector<std::shared_ptr<io_group>> groups;

    util::spinlock lock;

    io_queue_topology();
    io_queue_topology(const io_queue_topology&) = delete;
    io_queue_topology(io_queue_topology&&);
    ~io_queue_topology();
};

struct cpu {
    unsigned cpu_id;
    std::vector<memory> mem;
};

struct resources {
    std::vector<cpu> cpus;
    std::unordered_map<dev_t, io_queue_topology> ioq_topology;
    std::unordered_map<unsigned /* numa node id */, cpuset> numa_node_id_to_cpuset;
};

resources allocate(configuration& c);
unsigned nr_processing_units(configuration& c);

SEASTAR_MODULE_EXPORT_END

std::optional<resource::cpuset> parse_cpuset(std::string value);


}
}
