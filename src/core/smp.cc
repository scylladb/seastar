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
#include <seastar/core/reactor.hh>
#include <seastar/core/future.hh>
#include <seastar/core/semaphore.hh>
#include <boost/range/algorithm/find_if.hpp>
#include <vector>

namespace seastar {

struct smp_service_group_impl {
    std::vector<semaphore> clients;   // one client per server shard
};

static semaphore smp_service_group_management_sem{1};
static thread_local std::vector<smp_service_group_impl> smp_service_groups;

future<smp_service_group> create_smp_service_group(smp_service_group_config ssgc) {
    ssgc.max_nonlocal_requests = std::max(ssgc.max_nonlocal_requests, smp::count - 1);
    return smp::submit_to(0, [ssgc] {
        return with_semaphore(smp_service_group_management_sem, 1, [ssgc] {
            auto it = boost::range::find_if(smp_service_groups, [&] (smp_service_group_impl& ssgi) { return ssgi.clients.empty(); });
            size_t id = it - smp_service_groups.begin();
            return smp::invoke_on_all([ssgc, id] {
                if (id >= smp_service_groups.size()) {
                    smp_service_groups.resize(id + 1); // may throw
                }
                smp_service_groups[id].clients.reserve(smp::count); // may throw
                auto per_client = smp::count > 1 ? ssgc.max_nonlocal_requests / (smp::count - 1) : 0u;
                for (unsigned i = 0; i != smp::count; ++i) {
                    smp_service_groups[id].clients.emplace_back(per_client);
                }
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
                return smp_service_group(id);
            });
        });
    });
}

future<> destroy_smp_service_group(smp_service_group ssg) {
    return smp::submit_to(0, [ssg] {
        return with_semaphore(smp_service_group_management_sem, 1, [ssg] {
            auto id = internal::smp_service_group_id(ssg);
            return smp::invoke_on_all([id] {
                smp_service_groups[id].clients.clear();
            });
        });
    });
}

void init_default_smp_service_group() {
    smp_service_groups.emplace_back();
    auto& ssg0 = smp_service_groups.back();
    ssg0.clients.reserve(smp::count);
    for (unsigned i = 0; i != smp::count; ++i) {
        ssg0.clients.emplace_back(semaphore::max_counter());
    }
}

semaphore& get_smp_service_groups_semaphore(unsigned ssg_id, shard_id t) {
    return smp_service_groups[ssg_id].clients[t];
}

shard_id this_shard_id() {
    return engine().cpu_id();
}

}
