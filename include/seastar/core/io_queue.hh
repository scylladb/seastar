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

#pragma once

#include <seastar/core/sstring.hh>
#include <seastar/core/fair_queue.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/future.hh>
#include <unordered_map>

namespace seastar {

namespace internal {
namespace linux_abi {

struct io_event;

}
}

using shard_id = unsigned;

class io_priority_class;

class io_queue {
private:
    struct priority_class_data {
        priority_class_ptr ptr;
        size_t bytes;
        uint64_t ops;
        uint32_t nr_queued;
        std::chrono::duration<double> queue_time;
        metrics::metric_groups _metric_groups;
        priority_class_data(sstring name, sstring mountpoint, priority_class_ptr ptr, shard_id owner);
    };

    std::vector<std::vector<lw_shared_ptr<priority_class_data>>> _priority_classes;
    fair_queue _fq;

    static constexpr unsigned _max_classes = 2048;
    static std::mutex _register_lock;
    static std::array<uint32_t, _max_classes> _registered_shares;
    static std::array<sstring, _max_classes> _registered_names;

    static io_priority_class register_one_priority_class(sstring name, uint32_t shares);

    priority_class_data& find_or_create_class(const io_priority_class& pc, shard_id owner);
    friend smp;
public:
    enum class request_type { read, write };

    // We want to represent the fact that write requests are (maybe) more expensive
    // than read requests. To avoid dealing with floating point math we will scale one
    // read request to be counted by this amount.
    //
    // A write request that is 30% more expensive than a read will be accounted as
    // (read_request_base_count * 130) / 100.
    // It is also technically possible for reads to be the expensive ones, in which case
    // writes will have an integer value lower than read_request_base_count.
    static constexpr unsigned read_request_base_count = 128;

    struct config {
        shard_id coordinator;
        std::vector<shard_id> io_topology;
        unsigned capacity = std::numeric_limits<unsigned>::max();
        unsigned max_req_count = std::numeric_limits<unsigned>::max();
        unsigned max_bytes_count = std::numeric_limits<unsigned>::max();
        unsigned disk_req_write_to_read_multiplier = read_request_base_count;
        unsigned disk_bytes_write_to_read_multiplier = read_request_base_count;
        sstring mountpoint = "undefined";
    };

    io_queue(config cfg);
    ~io_queue();

    template <typename Func>
    future<internal::linux_abi::io_event>
    queue_request(const io_priority_class& pc, size_t len, request_type req_type, Func do_io);

    size_t capacity() const {
        return _config.capacity;
    }

    size_t queued_requests() const {
        return _fq.waiters();
    }

    // How many requests are sent to disk but not yet returned.
    size_t requests_currently_executing() const {
        return _fq.requests_currently_executing();
    }

    // Inform the underlying queue about the fact that some of our requests finished
    void notify_requests_finished(fair_queue_request_descriptor& desc) {
        _fq.notify_requests_finished(desc);
    }

    // Dispatch requests that are pending in the I/O queue
    void poll_io_queue() {
        _fq.dispatch_requests();
    }

    sstring mountpoint() const {
        return _config.mountpoint;
    }

    shard_id coordinator() const {
        return _config.coordinator;
    }
    shard_id coordinator_of_shard(shard_id shard) const {
        return _config.io_topology[shard];
    }

    future<> update_shares_for_class(io_priority_class pc, size_t new_shares);

    friend class reactor;
private:
    config _config;
    static fair_queue::config make_fair_queue_config(config cfg);
};

}
