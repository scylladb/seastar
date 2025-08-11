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
 * Copyright 2025 ScyllaDB
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <boost/range/adaptor/map.hpp>
#include <chrono>
#endif

#include <seastar/core/io_queue.hh>
#include <seastar/core/reactor_config.hh>

namespace seastar {

struct smp_options;

namespace internal {

SEASTAR_MODULE_EXPORT
struct disk_params {
    std::vector<std::string> mountpoints;
    std::vector<dev_t> devices;
    uint64_t read_bytes_rate = std::numeric_limits<uint64_t>::max();
    uint64_t write_bytes_rate = std::numeric_limits<uint64_t>::max();
    uint64_t read_req_rate = std::numeric_limits<uint64_t>::max();
    uint64_t write_req_rate = std::numeric_limits<uint64_t>::max();
    uint64_t read_saturation_length = std::numeric_limits<uint64_t>::max();
    uint64_t write_saturation_length = std::numeric_limits<uint64_t>::max();
    bool duplex = false;
    float rate_factor = 1.0;
    bool max_cost_function = true;
};

SEASTAR_MODULE_EXPORT
class disk_config_params {
private:
    const unsigned _max_queues;
    unsigned _num_io_groups = 0;
    std::unordered_map<unsigned, disk_params> _disks;
    std::chrono::duration<double> _latency_goal;
    std::chrono::milliseconds _stall_threshold;
    double _flow_ratio_backpressure_threshold;

public:
    explicit disk_config_params(unsigned max_queues) noexcept
            : _max_queues(max_queues)
    {}

    uint64_t per_io_group(uint64_t qty, unsigned nr_groups) const noexcept {
        return std::max(qty / nr_groups, 1ul);
    }

    unsigned num_io_groups() const noexcept { return _num_io_groups; }

    std::chrono::duration<double> latency_goal() const {
        return _latency_goal;
    }

    std::chrono::milliseconds stall_threshold() const {
        return _stall_threshold;
    }

    double latency_goal_opt(const reactor_options& opts) const {
        return opts.io_latency_goal_ms ?
                opts.io_latency_goal_ms.get_value() :
                opts.task_quota_ms.get_value() * 1.5;
    }

    void parse_config(const smp_options& smp_opts, const reactor_options& reactor_opts);

    struct io_queue::config generate_config(unsigned q, unsigned nr_groups) const;
    struct io_queue::config generate_config(const disk_params& p, unsigned q, unsigned nr_groups) const;

    auto queue_ids() {
        return boost::adaptors::keys(_disks);
    }

    const std::vector<dev_t>& queue_devices(unsigned q) const {
        return _disks.at(q).devices;
    }
};

}

}
