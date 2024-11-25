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
 * Copyright 2019 ScyllaDB
 */
#ifdef SEASTAR_MODULE
module;
#endif

#include <boost/range/algorithm/find_if.hpp>
#include <atomic>
#include <vector>
#include <regex>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/smp.hh>
#include <seastar/core/resource.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/print.hh>
#include <seastar/core/on_internal_error.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/align.hh>
#include "prefault.hh"
#endif

namespace seastar {

extern logger seastar_logger;

#ifdef SEASTAR_BUILD_SHARED_LIBS
shard_id* internal::this_shard_id_ptr() noexcept {
    static thread_local shard_id g_this_shard_id;
    return &g_this_shard_id;
}
#endif

void smp_message_queue::work_item::process() {
    schedule(this);
}

struct smp_service_group_impl {
    std::vector<smp_service_group_semaphore> clients;   // one client per server shard
#ifdef SEASTAR_DEBUG
    unsigned version = 0;
#endif
};

static thread_local smp_service_group_semaphore smp_service_group_management_sem{1, named_semaphore_exception_factory{"smp_service_group_management_sem"}};
static thread_local std::vector<smp_service_group_impl> smp_service_groups;

static named_semaphore_exception_factory make_service_group_semaphore_exception_factory(unsigned id, shard_id client_cpu, shard_id this_cpu, std::optional<sstring> smp_group_name) {
    if (smp_group_name) {
        return named_semaphore_exception_factory{format("smp_service_group:'{}' (#{}) {}->{} semaphore", *smp_group_name, id, client_cpu, this_cpu)};
    } else {
        return named_semaphore_exception_factory{format("smp_service_group#{} {}->{} semaphore", id, client_cpu, this_cpu)};
    }

}

static_assert(std::is_nothrow_copy_constructible_v<smp_service_group>);
static_assert(std::is_nothrow_move_constructible_v<smp_service_group>);

static_assert(std::is_nothrow_default_constructible_v<smp_submit_to_options>);
static_assert(std::is_nothrow_copy_constructible_v<smp_submit_to_options>);
static_assert(std::is_nothrow_move_constructible_v<smp_submit_to_options>);

future<smp_service_group> create_smp_service_group(smp_service_group_config ssgc) noexcept {
    ssgc.max_nonlocal_requests = std::max(ssgc.max_nonlocal_requests, smp::count - 1);
    return smp::submit_to(0, [ssgc] {
        return with_semaphore(smp_service_group_management_sem, 1, [ssgc] {
            auto it = boost::range::find_if(smp_service_groups, [&] (smp_service_group_impl& ssgi) { return ssgi.clients.empty(); });
            size_t id = it - smp_service_groups.begin();
            return parallel_for_each(smp::all_cpus(), [ssgc, id] (unsigned cpu) {
              return smp::submit_to(cpu, [ssgc, id, cpu] {
                if (id >= smp_service_groups.size()) {
                    smp_service_groups.resize(id + 1); // may throw
                }
                smp_service_groups[id].clients.reserve(smp::count); // may throw
                auto per_client = smp::count > 1 ? ssgc.max_nonlocal_requests / (smp::count - 1) : 0u;
                for (unsigned i = 0; i != smp::count; ++i) {
                    smp_service_groups[id].clients.emplace_back(per_client, make_service_group_semaphore_exception_factory(id, i, cpu, ssgc.group_name));
                }
              });
            }).handle_exception([id] (std::exception_ptr e) {
                // rollback
                return smp::invoke_on_all([id] {
                    if (smp_service_groups.size() > id) {
                        smp_service_groups[id].clients.clear();
                    }
                }).then([e = std::move(e)] () mutable {
                    std::rethrow_exception(std::move(e));
                });
            }).then([id] {
                auto ret = smp_service_group(id);
#ifdef SEASTAR_DEBUG
                ret._version = smp_service_groups[id].version;
#endif
                return ret;
            });
        });
    });
}

future<> destroy_smp_service_group(smp_service_group ssg) noexcept {
    return smp::submit_to(0, [ssg] {
        return with_semaphore(smp_service_group_management_sem, 1, [ssg] {
            auto id = internal::smp_service_group_id(ssg);
            if (id >= smp_service_groups.size()) {
                on_fatal_internal_error(seastar_logger, format("destroy_smp_service_group id={}: out of range", id));
            }
#ifdef SEASTAR_DEBUG
            if (ssg._version != smp_service_groups[id].version) {
                on_fatal_internal_error(seastar_logger, format("destroy_smp_service_group id={}: stale version={}: current_version={}", id, ssg._version, smp_service_groups[id].version));
            }
#endif
            return smp::invoke_on_all([id] {
                smp_service_groups[id].clients.clear();
#ifdef SEASTAR_DEBUG
                ++smp_service_groups[id].version;
#endif
            });
        });
    });
}

void init_default_smp_service_group(shard_id cpu) {
    // default_smp_service_group == smp_service_group(0) -> we assume service groups are empty
    // at this point. If they are not, it is quite possibly because we are running repeated
    // reactors in the program. Probably a test (see #2148).
    // This would be fine, we just create extra junk here, _but_ it is quite possible
    // that we actually run with different cpu count (see smp_options::smp), in which case
    // the `get_smp_service_groups_semaphore` below can cause us to return uninitialized memory.
    smp_service_groups.clear();
    smp_service_groups.emplace_back();
    auto& ssg0 = smp_service_groups.back();
    ssg0.clients.reserve(smp::count);
    for (unsigned i = 0; i != smp::count; ++i) {
        ssg0.clients.emplace_back(smp_service_group_semaphore::max_counter(), make_service_group_semaphore_exception_factory(0, i, cpu, {"default"}));
    }
}

smp_service_group_semaphore& get_smp_service_groups_semaphore(unsigned ssg_id, shard_id t) noexcept {
    return smp_service_groups[ssg_id].clients[t];
}

smp::smp(alien::instance& alien)
        : _alien(alien) {
}


smp::~smp() = default;

void
smp::setup_prefaulter(const seastar::resource::resources& res, seastar::memory::internal::numa_layout layout) {
    // Stack guards mprotect() random pages, so the prefaulter will hard-fault.
#ifndef SEASTAR_THREAD_STACK_GUARDS
    _prefaulter = std::make_unique<internal::memory_prefaulter>(res, std::move(layout));
#endif
}

static
std::optional<size_t>
get_huge_page_size() {
    auto meminfo_fd = file_desc::open("/proc/meminfo", O_RDONLY | O_CLOEXEC);
    std::string meminfo;
    char buf[4096];
    while (auto size_opt = meminfo_fd.read(buf, sizeof(buf))) {
        if (!*size_opt) {
            break;
        }
        meminfo.append(buf, *size_opt);
    }
    static std::regex re(R"(Hugepagesize:\s*(\d+) kB)");
    auto m = std::smatch{};
    if (std::regex_search(meminfo, m, re)) {
        return std::stoi(m[1]) * size_t(1024);
    }
    return std::nullopt;
}

internal::memory_prefaulter::memory_prefaulter(const resource::resources& res, memory::internal::numa_layout layout) {
    for (auto& range : layout.ranges) {
        _layout_by_node_id[range.numa_node_id].push_back(std::move(range));
    }
    auto page_size = getpagesize();
    auto huge_page_size_opt = get_huge_page_size();
    for (auto& numa_node_id_and_ranges : _layout_by_node_id) {
        auto& numa_node_id = numa_node_id_and_ranges.first;
        auto& ranges = numa_node_id_and_ranges.second;
        posix_thread::attr a;
        auto i = res.numa_node_id_to_cpuset.find(numa_node_id);
        if (i != res.numa_node_id_to_cpuset.end()) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            for (auto cpu : i->second) {
                CPU_SET(cpu, &cpuset);
            }
            a.set(cpuset);
        }
        _worker_threads.emplace_back(a, [this, &ranges, page_size, huge_page_size_opt] {
            work(ranges, page_size, huge_page_size_opt);
        });
    }
}

internal::memory_prefaulter::~memory_prefaulter() {
    _stop_request.store(true, std::memory_order_relaxed);
    for (auto& t : _worker_threads) {
        t.join();
    }
}

void
internal::memory_prefaulter::work(std::vector<memory::internal::memory_range>& ranges, size_t page_size,
        std::optional<size_t> huge_page_size_opt) {
    sched_param param = { .sched_priority = 0 };
    // SCHED_IDLE doesn't work via thread attributes
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &param);
    size_t current_range = 0;
    const size_t batch_size = huge_page_size_opt.value_or(512*4096);
    auto populate_memory_madvise = [works = true] (char* start, char* end) mutable {
#ifdef MADV_POPULATE_WRITE
        if (works) {
            auto result = madvise(start, end - start, MADV_POPULATE_WRITE);
            if (result == -1 && errno == EINVAL) {
                // Unsupported by kernel
                works = false;
                return false;
            }
            // Ignore other errors. This is just an optimization anyway.
#ifdef MADV_COLLAPSE
            madvise(start, end - start, MADV_COLLAPSE);
            // Also ignore problems with MADV_COLLAPSE, it's just an optimization.
#endif
            return true;
        };
#endif
        (void)works; // suppress warning if block above is elided
        return false;
    };
    auto fault_in_memory = [] (char* p) {
        // Touch the page for write, but be sure not to modify anything
        // The compilers tend to optimize things, so prefer assembly
#if defined(__x86_64__)
        asm volatile ("lock orb $0, %0" : "=&m"(*p));
#elif defined(__aarch64__)
        int byte; // ldxrb likes 32-bit registers
        int need_loop;
        asm volatile ("1: ldxrb %w0, %2; stxrb %w1, %w0, %2; cbnz %w1, 1b"
                : "=&r"(byte), "=&r"(need_loop), "+Q"(*p));
#else
        // atomic_ref would be better, but alas C++20 only
        auto p1 = reinterpret_cast<volatile std::atomic<char>*>(p);
        p1->fetch_or(0, std::memory_order_relaxed);
#endif
    };
    auto populate_memory = [&] (char* start, char* end) {
        if (populate_memory_madvise(start, end)) {
            return;
        }
        while (start < end) {
            fault_in_memory(start);
            start += page_size;
        }
    };
    while (!_stop_request.load(std::memory_order_relaxed) && !ranges.empty()) {
        auto& range = ranges[current_range];

        auto batch_end = std::min(align_up(range.start + 1, batch_size), range.end);
        populate_memory(range.start, batch_end);
        range.start = batch_end;

        // An end-to-start scan for applications that manage two heaps that
        // grow towards each other.
        auto batch_start = std::max(align_down(range.end - 1, batch_size), range.start);
        populate_memory(batch_start, range.end);
        range.end = batch_start;

        if (range.start >= range.end) {
            ranges.erase(ranges.begin() + current_range);
            current_range = 0;
        }
        current_range += 1;
        if (current_range >= ranges.size()) {
            current_range = 0;
        }
    }
}

}
