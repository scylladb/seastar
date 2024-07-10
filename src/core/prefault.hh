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

// Copyright 2023 ScyllaDB

#pragma once

#include <atomic>
#include <optional>
#include <unordered_map>
#include <vector>

#include <seastar/core/posix.hh>
#include <seastar/core/resource.hh>
#include <seastar/core/task.hh>
#include <seastar/core/memory.hh>

namespace seastar::internal {

// Responsible for pre-faulting in memory so soft page fault latency doesn't impact applications
class memory_prefaulter {
    std::atomic<bool> _stop_request = false;
    std::vector<posix_thread> _worker_threads;
    // Keep this in object scope to avoid allocating in worker thread
    std::unordered_map<unsigned, std::vector<memory::internal::memory_range>> _layout_by_node_id;
public:
    explicit memory_prefaulter(const resource::resources& res, memory::internal::numa_layout layout);
    ~memory_prefaulter();
private:
    void work(std::vector<memory::internal::memory_range>& ranges, size_t page_size, std::optional<size_t> huge_page_size_opt);
};


}

