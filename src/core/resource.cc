
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

#ifdef SEASTAR_MODULE
module;
#endif

#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/copy.hpp>
#include <ranges>
#include <regex>
#include <stdlib.h>
#include <unistd.h>
#include <limits>
#include <filesystem>
#include <unordered_map>
#include <fmt/core.h>
#include <seastar/util/assert.hh>
#if SEASTAR_HAVE_HWLOC
#include <hwloc/glibc-sched.h>
#endif

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/resource.hh>
#include <seastar/core/align.hh>
#include <seastar/core/print.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/read_first_line.hh>
#include <seastar/util/log.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/print.hh>
#include "cgroup.hh"

#endif

namespace seastar {

extern logger seastar_logger;

namespace resource {

// This function was made optional because of validate. It needs to
// throw an error when a non parseable input is given.
std::optional<cpuset> parse_cpuset(std::string value) {
    static std::regex r("(\\d+-)?(\\d+)(,(\\d+-)?(\\d+))*");

    std::smatch match;
    if (std::regex_match(value, match, r)) {
        std::vector<std::string> ranges;
        boost::split(ranges, value, boost::is_any_of(","));
        resource::cpuset ret;
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
                return std::nullopt;
            }

            for (auto i = b; i <= e; ++i) {
                ret.insert(i);
            }
        }
        return ret;
    }
    return std::nullopt;
}

}

namespace cgroup {

namespace fs = std::filesystem;

optional<cpuset> cpu_set() {
    auto cpuset = read_setting_V1V2_as<std::string>(
                              "cpuset/cpuset.cpus",
                              "cpuset.cpus.effective");
    if (cpuset) {
        return seastar::resource::parse_cpuset(*cpuset);
    }

    seastar_logger.warn("Unable to parse cgroup's cpuset. Ignoring.");
    return std::nullopt;
}

size_t memory_limit() {
    return read_setting_V1V2_as<size_t>(
                             "memory/memory.limit_in_bytes",
                             "memory.max")
        .value_or(std::numeric_limits<size_t>::max());
}

template <typename T>
optional<T> read_setting_as(std::string path) {
    try {
        auto line = read_first_line(path);
        return boost::lexical_cast<T>(line);
    } catch (...) {
        seastar_logger.warn("Couldn't read cgroup file {}.", path);
    }

    return std::nullopt;
}

/*
 * what cgroup do we belong to?
 *
 * For cgroups V2, /proc/self/cgroup should read "0::<cgroup-dir-path>"
 * Note: true only for V2-only systems, but there is no reason to support
 * a hybrid configuration.
 */
static optional<fs::path> cgroup2_path_my_pid() {
    seastar::sstring cline;
    try {
        cline = read_first_line(fs::path{"/proc/self/cgroup"});
    } catch (...) {
        // '/proc/self/cgroup' must be there. If not - there is an issue
        // with the system configuration.
        throw std::runtime_error("no cgroup data for our process");
    }

    // for a V2-only system, we expect exactly one line:
    // 0::<abs-path-to-cgroup>
    if (cline.at(0) != '0') {
        // This is either a v1 system, or system configured with a hybrid of v1 & v2.
        // We do not support such combinations of v1 and v2 at this point.
        seastar_logger.debug("Not a cgroups-v2-only system");
        return std::nullopt;
    }

    // the path is guaranteed to start with '0::/'
    return fs::path{"/sys/fs/cgroup/" + cline.substr(4)};
}

/*
 * traverse the cgroups V2 hierarchy bottom-up, starting from our process'
 * specific cgroup up to /sys/fs/cgroup, looking for the named file.
 */
static optional<fs::path> locate_lowest_cgroup2(fs::path lowest_subdir, std::string filename) {
    // locate the lowest subgroup containing the named file (i.e.
    // handles the requested control by itself)
    do {
        //  does the cgroup settings file exist?
        auto set_path = lowest_subdir / filename;
        if (fs::exists(set_path) ) {
            return set_path;
        }

        lowest_subdir = lowest_subdir.parent_path();
    } while (lowest_subdir.compare("/sys/fs"));

    return std::nullopt;
}

/*
 * Read a settings value from either the cgroups V2 or the corresponding
 * cgroups V1 files.
 * For V2, look for the lowest cgroup in our hierarchy that manages the
 * requested settings.
 */
template <typename T>
optional<T> read_setting_V1V2_as(std::string cg1_path, std::string cg2_fname) {
    // on v2-systems, cg2_path will be initialized with the leaf cgroup that
    // controls this process
    static optional<fs::path> cg2_path{cgroup2_path_my_pid()};

    if (cg2_path) {
        // this is a v2 system
        seastar::sstring line;
        try {
            line = read_first_line(locate_lowest_cgroup2(*cg2_path, cg2_fname).value());
        } catch (...) {
            seastar_logger.warn("Could not read cgroups v2 file ({}).", cg2_fname);
            return std::nullopt;
        }
        if (line.compare("max")) {
            try {
                return boost::lexical_cast<T>(line);
            } catch (...) {
                seastar_logger.warn("Malformed cgroups file ({}) contents.", cg2_fname);
            }
        }
        return std::nullopt;
    }

    // try cgroups v1:
    try {
        auto line = read_first_line(fs::path{"/sys/fs/cgroup"} / cg1_path);
        return boost::lexical_cast<T>(line);
    } catch (...) {
        seastar_logger.warn("Could not parse cgroups v1 file ({}).", cg1_path);
    }

    return std::nullopt;
}

}

namespace resource {

static unsigned long get_machine_memory_from_sysconf() {
    return ::sysconf(_SC_PAGESIZE) * size_t(::sysconf(_SC_PHYS_PAGES));
}

static
size_t
kernel_memory_reservation() {
    try {
        return read_first_line_as<size_t>("/proc/sys/vm/min_free_kbytes") * 1024;
    } catch (...) {
        return 0;
    }
}

size_t calculate_memory(const configuration& c, size_t available_memory, float panic_factor = 1) {
    auto kernel_reservation = kernel_memory_reservation();
    if (kernel_reservation >= 200'000'000) {
        // The standard setting is sqrt(mem)*128. This is 128MB at 1TB RAM. With 64kB pages and transparent hugepages,
        // the kernel increases this significantly, wasting memory.
        seastar_logger.warn("Kernel memory reservation (/proc/sys/vm/min_free_kbytes) unexpectedly high ({}), check your configuration", kernel_reservation);
    }
    available_memory -= kernel_reservation;
    size_t default_reserve_memory = std::max<size_t>(1536 * 1024 * 1024, 0.07 * available_memory) * panic_factor;
    auto reserve = c.reserve_memory.value_or(default_reserve_memory);
    auto reserve_additional = c.reserve_additional_memory_per_shard * c.cpus;
    reserve += reserve_additional;
    size_t min_memory = 500'000'000;
    if (available_memory >= reserve + min_memory) {
        available_memory -= reserve;
    } else {
        // Allow starting up even in low memory configurations (e.g. 2GB boot2docker VM)
        available_memory = min_memory;
    }
    if (!c.total_memory.has_value()) {
        return available_memory;
    }
    if (*c.total_memory < reserve_additional) {
        throw std::runtime_error(format("insufficient total memory: reserve {} total {}", reserve_additional, *c.total_memory));
    }
    size_t needed_memory = *c.total_memory - reserve_additional;
    if (needed_memory > available_memory) {
        throw std::runtime_error(format("insufficient physical memory: needed {} available {}", needed_memory, available_memory));
    }
    return needed_memory;
}

io_queue_topology::io_queue_topology() {
}

io_queue_topology::~io_queue_topology() {
}

io_queue_topology::io_queue_topology(io_queue_topology&& o)
    : queues(std::move(o.queues))
    , shard_to_group(std::move(o.shard_to_group))
    , shards_in_group(std::move(o.shards_in_group))
    , groups(std::move(o.groups))
    , lock() // unused until now, so just initialize
{ }

}

}

#ifdef SEASTAR_HAVE_HWLOC

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

static hwloc_uint64_t get_memory_from_hwloc_obj(hwloc_obj_t obj) {
#if HWLOC_API_VERSION >= 0x00020000
    auto total_memory = obj->total_memory;
#else
    auto total_memory = obj->memory.total_memory;
#endif
    return total_memory;
}

static void set_memory_to_hwloc_obj(hwloc_obj_t machine, hwloc_uint64_t memory) {
#if HWLOC_API_VERSION >= 0x00020000
    machine->total_memory = memory;
#else
    machine->memory.total_memory = memory;
#endif
}

static size_t alloc_from_node(cpu& this_cpu, hwloc_obj_t node, std::unordered_map<hwloc_obj_t, size_t>& used_mem, size_t alloc) {
    auto local_memory = get_memory_from_hwloc_obj(node);
    auto taken = std::min(local_memory - used_mem[node], alloc);
    if (taken) {
        used_mem[node] += taken;
        auto node_id = hwloc_bitmap_first(node->nodeset);
        SEASTAR_ASSERT(node_id != -1);
        this_cpu.mem.push_back({taken, unsigned(node_id)});
    }
    return taken;
}

// Find the numa node that contains a specific PU.
static hwloc_obj_t get_numa_node_for_pu(hwloc_topology_t topology, hwloc_obj_t pu) {
    // Can't use ancestry because hwloc 2.0 NUMA nodes are not ancestors of PUs
    hwloc_obj_t tmp = NULL;
    auto depth = hwloc_get_type_or_above_depth(topology, HWLOC_OBJ_NUMANODE);
    while ((tmp = hwloc_get_next_obj_by_depth(topology, depth, tmp)) != NULL) {
        if (hwloc_bitmap_intersects(tmp->cpuset, pu->cpuset)) {
            return tmp;
        }
    }
    return nullptr;
}

static hwloc_obj_t hwloc_get_ancestor(hwloc_obj_type_t type, hwloc_topology_t topology, unsigned cpu_id) {
    auto cur = hwloc_get_pu_obj_by_os_index(topology, cpu_id);

    while (cur != nullptr) {
        if (cur->type == type) {
            break;
        }
        cur = cur->parent;
    }

    return cur;
}

static std::unordered_map<hwloc_obj_t, std::vector<unsigned>> break_cpus_into_groups(hwloc_topology_t topology,
        std::vector<unsigned> cpus, hwloc_obj_type_t type) {
    std::unordered_map<hwloc_obj_t, std::vector<unsigned>> groups;

    for (auto&& cpu_id : cpus) {
        hwloc_obj_t anc = hwloc_get_ancestor(type, topology, cpu_id);
        groups[anc].push_back(cpu_id);
    }

    return groups;
}

struct distribute_objects {
    std::vector<hwloc_cpuset_t> cpu_sets;
    hwloc_obj_t root;

    distribute_objects(hwloc_topology_t topology, size_t nobjs) : cpu_sets(nobjs), root(hwloc_get_root_obj(topology)) {
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
allocate_io_queues(hwloc_topology_t topology, std::vector<cpu> cpus, std::unordered_map<unsigned, hwloc_obj_t>& cpu_to_node,
        unsigned num_io_groups, unsigned& last_node_idx) {
    auto node_of_shard = [&cpus, &cpu_to_node] (unsigned shard) {
        auto node = cpu_to_node.at(cpus[shard].cpu_id);
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
    for (auto shard: std::views::iota(0, int(cpus.size()))) {
        auto node_id = node_of_shard(shard);

        if (numa_nodes.count(node_id) == 0) {
            numa_nodes.emplace(node_id, std::set<unsigned>());
        }
        numa_nodes.at(node_id).insert(shard);
    }

    io_queue_topology ret;
    ret.shard_to_group.resize(cpus.size());
    ret.shards_in_group.resize(cpus.size(), 0); // worst case

    if (num_io_groups == 0) {
        num_io_groups = numa_nodes.size();
        SEASTAR_ASSERT(num_io_groups != 0);
        seastar_logger.debug("Auto-configure {} IO groups", num_io_groups);
    } else if (num_io_groups > cpus.size()) {
        // User may be playing with --smp option, but num_io_groups was independently
        // determined by iotune, so adjust for any conflicts.
        fmt::print("Warning: number of IO queues ({:d}) greater than logical cores ({:d}). Adjusting downwards.\n", num_io_groups, cpus.size());
        num_io_groups = cpus.size();
    }

    auto find_shard = [&cpus] (unsigned cpu_id) {
        auto idx = 0u;
        for (auto& c: cpus) {
            if (c.cpu_id == cpu_id) {
                return idx;
            }
            idx++;
        }
        SEASTAR_ASSERT(0);
    };

    auto cpu_sets = distribute_objects(topology, num_io_groups);
    ret.queues.resize(cpus.size());
    unsigned nr_groups = 0;

    // First step: distribute the IO queues given the information returned in cpu_sets.
    // If there is one IO queue per processor, only this loop will be executed.
    std::unordered_map<unsigned, std::vector<unsigned>> node_coordinators;
    for (auto&& cs : cpu_sets()) {
        auto io_coordinator = find_shard(hwloc_bitmap_first(cs));
        unsigned group_idx = nr_groups++;
        ret.shard_to_group[io_coordinator] = group_idx;
        ret.shards_in_group[group_idx]++;

        auto node_id = node_of_shard(io_coordinator);
        if (node_coordinators.count(node_id) == 0) {
            node_coordinators.emplace(node_id, std::vector<unsigned>());
        }
        node_coordinators.at(node_id).push_back(io_coordinator);
        numa_nodes[node_id].erase(io_coordinator);
    }

    ret.groups.resize(nr_groups);

    auto available_nodes = boost::copy_range<std::vector<unsigned>>(node_coordinators | boost::adaptors::map_keys);

    // If there are more processors than coordinators, we will have to assign them to existing
    // coordinators. We prefer do that within the same NUMA node, but if not possible we assign
    // the shard to a random node.
    for (auto& node: numa_nodes) {
        auto cid_idx = 0;
        for (auto& remaining_shard: node.second) {
            auto my_node = node.first;
            // No I/O queue in this node, round-robin shards from this node into existing ones.
            if (!node_coordinators.count(node.first)) {
                my_node = available_nodes[last_node_idx++ % available_nodes.size()];
            }
            auto idx = cid_idx++ % node_coordinators.at(my_node).size();
            auto io_coordinator = node_coordinators.at(my_node)[idx];
            unsigned group_idx = ret.shard_to_group[io_coordinator];
            ret.shard_to_group[remaining_shard] = group_idx;
            ret.shards_in_group[group_idx]++;
        }
    }

    return ret;
}

namespace hwloc::internal {

topology_holder::topology_holder(topology_holder&& o) noexcept
    : _topology(std::exchange(o._topology, nullptr))
{ }

topology_holder::~topology_holder() {
    if (_topology) {
        hwloc_topology_destroy(_topology);
    }
}

topology_holder& topology_holder::operator=(topology_holder&& o) noexcept {
    if (this != &o) {
        std::swap(_topology, o._topology);
    }
    return *this;
}

void topology_holder::init_and_load() {
    hwloc_topology_init(&_topology);
    // hwloc_topology_destroy is required after hwloc_topology_init
    // on success, _topology will not be null anymore

    hwloc_topology_load(_topology);
}

hwloc_topology_t topology_holder::get() {
    if (!_topology) {
        init_and_load();
    }
    return _topology;
}

} // namespace hwloc::internal

static
std::unordered_map<unsigned, cpuset>
numa_node_id_to_cpuset(hwloc_topology_t topo) {
    auto ret = std::unordered_map<unsigned, cpuset>();
    for (auto numa_node = hwloc_get_next_obj_by_type(topo, HWLOC_OBJ_NUMANODE, NULL);
            numa_node;
            numa_node = hwloc_get_next_obj_by_type(topo, HWLOC_OBJ_NUMANODE, numa_node)) {
        auto parent = numa_node->parent;
        auto cpuset = parent->cpuset;
        cpu_set_t os_cpuset;
        hwloc_cpuset_to_glibc_sched_affinity(topo, cpuset, &os_cpuset, sizeof(os_cpuset));
        for (unsigned idx = 0; idx < CPU_SETSIZE; ++idx) {
            if (CPU_ISSET(idx, &os_cpuset)) {
                ret[numa_node->os_index].insert(idx);
            }
        }
    }
    return ret;
}

resources allocate(configuration& c) {
    auto topology = c.topology.get();
    auto bm = hwloc_bitmap_alloc();
    auto free_bm = defer([&] () noexcept { hwloc_bitmap_free(bm); });
    for (auto idx : c.cpu_set) {
        hwloc_bitmap_set(bm, idx);
    }
    auto r = hwloc_topology_restrict(topology, bm,
#if HWLOC_API_VERSION >= 0x00020000
            0
#else
            HWLOC_RESTRICT_FLAG_ADAPT_DISTANCES
#endif
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
    unsigned procs = c.cpus;
    if (unsigned available_procs = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PU);
        procs > available_procs) {
        throw std::runtime_error(format("insufficient processing units: needed {} available {}", procs, available_procs));
    }
    if (procs == 0) {
        throw std::runtime_error("number of processing units must be positive");
    }
    auto machine_depth = hwloc_get_type_depth(topology, HWLOC_OBJ_MACHINE);
    SEASTAR_ASSERT(hwloc_get_nbobjs_by_depth(topology, machine_depth) == 1);
    auto machine = hwloc_get_obj_by_depth(topology, machine_depth, 0);
    auto available_memory = get_memory_from_hwloc_obj(machine);
    if (!available_memory) {
        available_memory = get_machine_memory_from_sysconf();
        set_memory_to_hwloc_obj(machine, available_memory);
        seastar_logger.warn("hwloc failed to detect machine-wide memory size, using memory size fetched from sysconf");
    }

    size_t mem = calculate_memory(c, std::min(available_memory,
                                              cgroup::memory_limit()));
    // limit memory address to fit in 36-bit, see core/memory.cc:Memory map
    constexpr size_t max_mem_per_proc = 1UL << 36;
    auto mem_per_proc = std::min(align_down<size_t>(mem / procs, 2 << 20), max_mem_per_proc);

    resources ret;
    std::unordered_map<unsigned, hwloc_obj_t> cpu_to_node;
    std::vector<unsigned> orphan_pus;
    std::unordered_map<hwloc_obj_t, size_t> topo_used_mem;
    std::vector<std::pair<cpu, size_t>> remains;

    auto cpu_sets = distribute_objects(topology, procs);
    auto num_nodes = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_NUMANODE);

    for (auto&& cs : cpu_sets()) {
        auto cpu_id = hwloc_bitmap_first(cs);
        SEASTAR_ASSERT(cpu_id != -1);
        auto pu = hwloc_get_pu_obj_by_os_index(topology, cpu_id);
        auto node = get_numa_node_for_pu(topology, pu);
        if (node == nullptr) {
            orphan_pus.push_back(cpu_id);
        } else {
            if (!get_memory_from_hwloc_obj(node)) {
                // If hwloc fails to detect the hardware topology, it falls back to treating
                // the system as a single-node configuration. While this code supports
                // multi-node setups, the fallback behavior is safe and will function
                // correctly in this case.
                SEASTAR_ASSERT(num_nodes == 1);
                auto local_memory = get_machine_memory_from_sysconf();
                set_memory_to_hwloc_obj(node, local_memory);
                seastar_logger.warn("hwloc failed to detect NUMA node memory size, using memory size fetched from sysfs");
            }
            cpu_to_node[cpu_id] = node;
            seastar_logger.debug("Assign CPU{} to NUMA{}", cpu_id, node->os_index);
        }
    }

    if (!orphan_pus.empty()) {
        if (!c.assign_orphan_cpus) {
            seastar_logger.error("CPUs without local NUMA nodes are disabled by the "
                        "--allow-cpus-in-remote-numa-nodes=false option.\n");
            throw std::runtime_error("no NUMA node for CPU");
        }

        seastar_logger.warn("Assigning some CPUs to remote NUMA nodes");

        // Get the list of NUMA nodes available
        std::vector<hwloc_obj_t> nodes;

        hwloc_obj_t tmp = NULL;
        auto depth = hwloc_get_type_or_above_depth(topology, HWLOC_OBJ_NUMANODE);
        while ((tmp = hwloc_get_next_obj_by_depth(topology, depth, tmp)) != NULL) {
            nodes.push_back(tmp);
        }

        // Group orphan CPUs by ... some sane enough feature
        std::unordered_map<hwloc_obj_t, std::vector<unsigned>> grouped;
        hwloc_obj_type_t group_by[] = {
            HWLOC_OBJ_L3CACHE,
            HWLOC_OBJ_L2CACHE,
            HWLOC_OBJ_L1CACHE,
            HWLOC_OBJ_PU,
        };

        for (auto&& gb : group_by) {
            grouped = break_cpus_into_groups(topology, orphan_pus, gb);
            if (grouped.size() >= nodes.size()) {
                seastar_logger.debug("Grouped orphan CPUs by {}", hwloc_obj_type_string(gb));
                break;
            }
            // Try to scatter orphans into as much NUMA nodes as possible
            // by grouping them with more specific selection
        }

        // Distribute PUs among the nodes by groups
        unsigned nid = 0;
        for (auto&& grp : grouped) {
            for (auto&& cpu_id : grp.second) {
                cpu_to_node[cpu_id] = nodes[nid];
                seastar_logger.debug("Assign orphan CPU{} to NUMA{}", cpu_id, nodes[nid]->os_index);
            }
            nid = (nid + 1) % nodes.size();
        }
    }

    // Divide local memory to cpus
    for (auto&& cs : cpu_sets()) {
        auto cpu_id = hwloc_bitmap_first(cs);
        SEASTAR_ASSERT(cpu_id != -1);
        auto node = cpu_to_node.at(cpu_id);
        cpu this_cpu;
        this_cpu.cpu_id = cpu_id;
        size_t remain = mem_per_proc - alloc_from_node(this_cpu, node, topo_used_mem, mem_per_proc);

        remains.emplace_back(std::move(this_cpu), remain);
    }

    // Divide the rest of the memory
    auto depth = hwloc_get_type_or_above_depth(topology, HWLOC_OBJ_NUMANODE);
    for (auto&& [this_cpu, remain] : remains) {
        auto node = cpu_to_node.at(this_cpu.cpu_id);
        auto obj = node;

        while (remain) {
            remain -= alloc_from_node(this_cpu, obj, topo_used_mem, remain);
            do {
                obj = hwloc_get_next_obj_by_depth(topology, depth, obj);
            } while (!obj);
            if (obj == node)
                break;
        }
        SEASTAR_ASSERT(!remain);
        ret.cpus.push_back(std::move(this_cpu));
    }

    unsigned last_node_idx = 0;
    for (auto devid : c.devices) {
        ret.ioq_topology.emplace(devid, allocate_io_queues(topology, ret.cpus, cpu_to_node, c.num_io_groups, last_node_idx));
    }

    ret.numa_node_id_to_cpuset = numa_node_id_to_cpuset(topology);

    return ret;
}

unsigned nr_processing_units(configuration& c) {
    return hwloc_get_nbobjs_by_type(c.topology.get(), HWLOC_OBJ_PU);
}

}

}

#else

namespace seastar {

namespace resource {

// Without hwloc, we don't support tuning the number of IO queues. So each CPU gets their.
static io_queue_topology
allocate_io_queues(configuration c, std::vector<cpu> cpus) {
    io_queue_topology ret;

    unsigned nr_cpus = unsigned(cpus.size());
    ret.queues.resize(nr_cpus);
    ret.shard_to_group.resize(nr_cpus);
    ret.shards_in_group.resize(1, 0);
    ret.groups.resize(1);

    for (unsigned shard = 0; shard < nr_cpus; ++shard) {
        ret.shard_to_group[shard] = 0;
        ret.shards_in_group[0]++;
    }
    return ret;
}


resources allocate(configuration& c) {
    resources ret;

    auto available_memory = get_machine_memory_from_sysconf();
    auto mem = calculate_memory(c, available_memory);
    auto procs = c.cpus;
    ret.cpus.reserve(procs);
    // limit memory address to fit in 36-bit, see core/memory.cc:Memory map
    constexpr size_t max_mem_per_proc = 1UL << 36;
    auto mem_per_proc = std::min(mem / procs, max_mem_per_proc);
    for (auto cpuid : c.cpu_set) {
        ret.cpus.push_back(cpu{cpuid, {{mem_per_proc, 0}}});
    }

    ret.ioq_topology.emplace(0, allocate_io_queues(c, ret.cpus));
    return ret;
}

unsigned nr_processing_units(configuration&) {
    return ::sysconf(_SC_NPROCESSORS_ONLN);
}

}

}

#endif
