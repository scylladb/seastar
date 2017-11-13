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

#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <regex>
#include "resource.hh"
#include "core/align.hh"
#include "core/print.hh"

namespace seastar {

// Overload for boost program options parsing/validation
void validate(boost::any& v,
              const std::vector<std::string>& values,
              cpuset_bpo_wrapper* target_type, int) {
    using namespace boost::program_options;
    static std::regex r("(\\d+-)?(\\d+)(,(\\d+-)?(\\d+))*");
    validators::check_first_occurrence(v);
    // Extract the first string from 'values'. If there is more than
    // one string, it's an error, and exception will be thrown.
    auto&& s = validators::get_single_string(values);
    std::smatch match;
    if (std::regex_match(s, match, r)) {
        std::vector<std::string> ranges;
        boost::split(ranges, s, boost::is_any_of(","));
        cpuset_bpo_wrapper ret;
        for (auto&& range: ranges) {
            std::string beg = range;
            std::string end = range;
            auto dash = range.find('-');
            if (dash != range.npos) {
                beg = range.substr(0, dash);
                end = range.substr(dash + 1);
            }
            auto b = boost::lexical_cast<unsigned>(beg);
            auto e = boost::lexical_cast<unsigned>(end);
            if (b > e) {
                throw validation_error(validation_error::invalid_option_value);
            }
            for (auto i = b; i <= e; ++i) {
                ret.value.insert(i);
            }
        }
        v = std::move(ret);
    } else {
        throw validation_error(validation_error::invalid_option_value);
    }
}

namespace resource {

size_t calculate_memory(configuration c, size_t available_memory, float panic_factor = 1) {
    size_t default_reserve_memory = std::max<size_t>(1536 * 1024 * 1024, 0.07 * available_memory) * panic_factor;
    auto reserve = c.reserve_memory.value_or(default_reserve_memory);
    size_t min_memory = 500'000'000;
    if (available_memory >= reserve + min_memory) {
        available_memory -= reserve;
    } else {
        // Allow starting up even in low memory configurations (e.g. 2GB boot2docker VM)
        available_memory = min_memory;
    }
    size_t mem = c.total_memory.value_or(available_memory);
    if (mem > available_memory) {
        throw std::runtime_error(format("insufficient physical memory: needed {} available {}", mem, available_memory));
    }
    return mem;
}

}

}

#ifdef HAVE_HWLOC

#include "util/defer.hh"
#include "core/print.hh"
#include <hwloc.h>
#include <unordered_map>
#include <boost/range/irange.hpp>

namespace seastar {

cpu_set_t cpuid_to_cpuset(unsigned cpuid) {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpuid, &cs);
    return cs;
}

namespace resource {

size_t div_roundup(size_t num, size_t denom) {
    return (num + denom - 1) / denom;
}

static unsigned find_memory_depth(hwloc_topology_t& topology) {
    auto depth = hwloc_get_type_depth(topology, HWLOC_OBJ_PU);
    auto obj = hwloc_get_next_obj_by_depth(topology, depth, nullptr);

    while (!obj->memory.local_memory && obj) {
        obj = hwloc_get_ancestor_obj_by_depth(topology, --depth, obj);
    }
    assert(obj);
    return depth;
}

static size_t alloc_from_node(cpu& this_cpu, hwloc_obj_t node, std::unordered_map<hwloc_obj_t, size_t>& used_mem, size_t alloc) {
    auto taken = std::min(node->memory.local_memory - used_mem[node], alloc);
    if (taken) {
        used_mem[node] += taken;
        auto node_id = hwloc_bitmap_first(node->nodeset);
        assert(node_id != -1);
        this_cpu.mem.push_back({taken, unsigned(node_id)});
    }
    return taken;
}

struct distribute_objects {
    std::vector<hwloc_cpuset_t> cpu_sets;
    hwloc_obj_t root;

    distribute_objects(hwloc_topology_t& topology, size_t nobjs) : cpu_sets(nobjs), root(hwloc_get_root_obj(topology)) {
#if HWLOC_API_VERSION >= 0x00010900
        hwloc_distrib(topology, &root, 1, cpu_sets.data(), cpu_sets.size(), INT_MAX, 0);
#else
        hwloc_distribute(topology, root, cpu_sets.data(), cpu_sets.size(), INT_MAX);
#endif
    }

    ~distribute_objects() {
        for (auto&& cs : cpu_sets) {
            hwloc_bitmap_free(cs);
        }
    }
    std::vector<hwloc_cpuset_t>& operator()() {
        return cpu_sets;
    }
};

static io_queue_topology
allocate_io_queues(hwloc_topology_t& topology, configuration c, std::vector<cpu> cpus) {
    unsigned num_io_queues = c.io_queues.value_or(cpus.size());
    unsigned max_io_requests = c.max_io_requests.value_or(128 * num_io_queues);

    unsigned depth = find_memory_depth(topology);
    auto node_of_shard = [&topology, &cpus, &depth] (unsigned shard) {
        auto pu = hwloc_get_pu_obj_by_os_index(topology, cpus[shard].cpu_id);
        auto node = hwloc_get_ancestor_obj_by_depth(topology, depth, pu);
        return hwloc_bitmap_first(node->nodeset);
    };

    // There are two things we are trying to achieve by populating a numa_nodes map.
    //
    // The first is to find out how many nodes we have in the system. We can't use
    // hwloc for that, because at this point we are not longer talking about the physical system,
    // but the actual booted seastar server instead. So if we have restricted the run to a subset
    // of the available processors, counting topology nodes won't spur the same result.
    //
    // Secondly, we need to find out which processors live in each node. For a reason similar to the
    // above, hwloc won't do us any good here. Later on, we will use this information to assign
    // shards to coordinators that are node-local to themselves.
    std::unordered_map<unsigned, std::set<unsigned>> numa_nodes;
    for (auto shard: boost::irange(0, int(cpus.size()))) {
        auto node_id = node_of_shard(shard);

        if (numa_nodes.count(node_id) == 0) {
            numa_nodes.emplace(node_id, std::set<unsigned>());
        }
        numa_nodes.at(node_id).insert(shard);
    }

    io_queue_topology ret;
    ret.shard_to_coordinator.resize(cpus.size());

    // User may be playing with --smp option, but num_io_queues was independently
    // determined by iotune, so adjust for any conflicts.
    if (num_io_queues > cpus.size()) {
        print("Warning: number of IO queues (%d) greater than logical cores (%d). Adjusting downwards.\n", num_io_queues, cpus.size());
        num_io_queues = cpus.size();
    }

    auto find_shard = [&cpus] (unsigned cpu_id) {
        auto idx = 0u;
        for (auto& c: cpus) {
            if (c.cpu_id == cpu_id) {
                return idx;
            }
            idx++;
        }
        assert(0);
    };

    auto cpu_sets = distribute_objects(topology, num_io_queues);
    // First step: distribute the IO queues given the information returned in cpu_sets.
    // If there is one IO queue per processor, only this loop will be executed.
    std::unordered_map<unsigned, std::vector<unsigned>> node_coordinators;
    for (auto&& cs : cpu_sets()) {
        auto io_coordinator = find_shard(hwloc_bitmap_first(cs));

        ret.coordinators.emplace_back(io_queue{io_coordinator, std::max(max_io_requests / num_io_queues , 1u)});
        // If a processor is a coordinator, it is also obviously a coordinator of itself
        ret.shard_to_coordinator[io_coordinator] = io_coordinator;

        auto node_id = node_of_shard(io_coordinator);
        if (node_coordinators.count(node_id) == 0) {
            node_coordinators.emplace(node_id, std::vector<unsigned>());
        }
        node_coordinators.at(node_id).push_back(io_coordinator);
        numa_nodes[node_id].erase(io_coordinator);
    }

    // If there are more processors than coordinators, we will have to assign them to existing
    // coordinators. We always do that within the same NUMA node.
    for (auto& node: numa_nodes) {
        auto cid_idx = 0;
        for (auto& remaining_shard: node.second) {
            auto idx = cid_idx++ % node_coordinators.at(node.first).size();
            auto io_coordinator = node_coordinators.at(node.first)[idx];
            ret.shard_to_coordinator[remaining_shard] = io_coordinator;
        }
    }

    return ret;
}


resources allocate(configuration c) {
    hwloc_topology_t topology;
    hwloc_topology_init(&topology);
    auto free_hwloc = defer([&] { hwloc_topology_destroy(topology); });
    hwloc_topology_load(topology);
    if (c.cpu_set) {
        auto bm = hwloc_bitmap_alloc();
        auto free_bm = defer([&] { hwloc_bitmap_free(bm); });
        for (auto idx : *c.cpu_set) {
            hwloc_bitmap_set(bm, idx);
        }
        auto r = hwloc_topology_restrict(topology, bm,
                HWLOC_RESTRICT_FLAG_ADAPT_DISTANCES
                | HWLOC_RESTRICT_FLAG_ADAPT_MISC
                | HWLOC_RESTRICT_FLAG_ADAPT_IO);
        if (r == -1) {
            if (errno == ENOMEM) {
                throw std::bad_alloc();
            }
            if (errno == EINVAL) {
                throw std::runtime_error("bad cpuset");
            }
            abort();
        }
    }
    auto machine_depth = hwloc_get_type_depth(topology, HWLOC_OBJ_MACHINE);
    assert(hwloc_get_nbobjs_by_depth(topology, machine_depth) == 1);
    auto machine = hwloc_get_obj_by_depth(topology, machine_depth, 0);
    auto available_memory = machine->memory.total_memory;
    size_t mem = calculate_memory(c, available_memory);
    unsigned available_procs = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);
    unsigned procs = c.cpus.value_or(available_procs);
    if (procs > available_procs) {
        throw std::runtime_error("insufficient processing units");
    }
    auto mem_per_proc = align_down<size_t>(mem / procs, 2 << 20);

    resources ret;
    std::unordered_map<hwloc_obj_t, size_t> topo_used_mem;
    std::vector<std::pair<cpu, size_t>> remains;
    size_t remain;
    unsigned depth = find_memory_depth(topology);

    auto cpu_sets = distribute_objects(topology, procs);

    // Divide local memory to cpus
    for (auto&& cs : cpu_sets()) {
        auto cpu_id = hwloc_bitmap_first(cs);
        assert(cpu_id != -1);
        auto pu = hwloc_get_pu_obj_by_os_index(topology, cpu_id);
        auto node = hwloc_get_ancestor_obj_by_depth(topology, depth, pu);
        cpu this_cpu;
        this_cpu.cpu_id = cpu_id;
        remain = mem_per_proc - alloc_from_node(this_cpu, node, topo_used_mem, mem_per_proc);

        remains.emplace_back(std::move(this_cpu), remain);
    }

    // Divide the rest of the memory
    for (auto&& r : remains) {
        cpu this_cpu;
        size_t remain;
        std::tie(this_cpu, remain) = r;
        auto pu = hwloc_get_pu_obj_by_os_index(topology, this_cpu.cpu_id);
        auto node = hwloc_get_ancestor_obj_by_depth(topology, depth, pu);
        auto obj = node;

        while (remain) {
            remain -= alloc_from_node(this_cpu, obj, topo_used_mem, remain);
            do {
                obj = hwloc_get_next_obj_by_depth(topology, depth, obj);
            } while (!obj);
            if (obj == node)
                break;
        }
        assert(!remain);
        ret.cpus.push_back(std::move(this_cpu));
    }

    ret.io_queues = allocate_io_queues(topology, c, ret.cpus);
    return ret;
}

unsigned nr_processing_units() {
    hwloc_topology_t topology;
    hwloc_topology_init(&topology);
    auto free_hwloc = defer([&] { hwloc_topology_destroy(topology); });
    hwloc_topology_load(topology);
    return hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);
}

}

}

#else

#include "resource.hh"
#include <unistd.h>

namespace seastar {

namespace resource {

// Without hwloc, we don't support tuning the number of IO queues. So each CPU gets their.
static io_queue_topology
allocate_io_queues(configuration c, std::vector<cpu> cpus) {
    io_queue_topology ret;

    unsigned nr_cpus = unsigned(cpus.size());
    unsigned max_io_requests = c.max_io_requests.value_or(128 * nr_cpus);

    ret.shard_to_coordinator.resize(nr_cpus);
    ret.coordinators.resize(nr_cpus);

    for (unsigned shard = 0; shard < nr_cpus; ++shard) {
        ret.shard_to_coordinator[shard] = shard;
        ret.coordinators[shard].capacity =  std::max(max_io_requests / nr_cpus, 1u);
        ret.coordinators[shard].id = shard;
    }
    return ret;
}


resources allocate(configuration c) {
    resources ret;

    auto available_memory = ::sysconf(_SC_PAGESIZE) * size_t(::sysconf(_SC_PHYS_PAGES));
    auto mem = calculate_memory(c, available_memory);
    auto cpuset_procs = c.cpu_set ? c.cpu_set->size() : nr_processing_units();
    auto procs = c.cpus.value_or(cpuset_procs);
    ret.cpus.reserve(procs);
    for (unsigned i = 0; i < procs; ++i) {
        ret.cpus.push_back(cpu{i, {{mem / procs, 0}}});
    }

    ret.io_queues = allocate_io_queues(c, ret.cpus);
    return ret;
}

unsigned nr_processing_units() {
    return ::sysconf(_SC_NPROCESSORS_ONLN);
}

}

}

#endif
