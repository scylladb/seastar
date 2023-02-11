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

#include <seastar/core/smp.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/print.hh>
#include <seastar/core/on_internal_error.hh>
#include <boost/range/algorithm/find_if.hpp>
#include <vector>

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

static smp_service_group_semaphore smp_service_group_management_sem{1, named_semaphore_exception_factory{"smp_service_group_management_sem"}};
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

}
