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
#include <seastar/core/internal/io_request.hh>
#include <mutex>
#include <array>

namespace seastar {

class io_priority_class;

/// Renames an io priority class
///
/// Renames an \ref io_priority_class previously created with register_one_priority_class().
///
/// The operation is global and affects all shards.
/// The operation affects the exported statistics labels.
///
/// \param pc The io priority class to be renamed
/// \param new_name The new name for the io priority class
/// \return a future that is ready when the io priority class have been renamed
future<>
rename_priority_class(io_priority_class pc, sstring new_name);

namespace internal {
namespace linux_abi {

struct io_event;
struct iocb;

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
        void rename(sstring new_name, sstring mountpoint, shard_id owner);
    private:
        void register_stats(sstring name, sstring mountpoint, shard_id owner);
    };

    std::vector<std::vector<lw_shared_ptr<priority_class_data>>> _priority_classes;
    fair_queue _fq;

    static constexpr unsigned _max_classes = 2048;
    static std::mutex _register_lock;
    static std::array<uint32_t, _max_classes> _registered_shares;
    static std::array<sstring, _max_classes> _registered_names;

    static io_priority_class register_one_priority_class(sstring name, uint32_t shares);

    priority_class_data& find_or_create_class(const io_priority_class& pc, shard_id owner);
    friend class smp;
    fair_queue_ticket _completed_accumulator = { 0, 0 };

    // The fields below are going away, they are just here so we can implement deprecated
    // functions that used to be provided by the fair_queue and are going away (from both
    // the fair_queue and the io_queue). Double-accounting for now will allow for easier
    // decoupling and is temporary
    size_t _queued_requests = 0;
    size_t _requests_executing = 0;
public:
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

    future<size_t>
    queue_request(const io_priority_class& pc, size_t len, internal::io_request req) noexcept;

    [[deprecated("modern I/O queues should use a property file")]] size_t capacity() const {
        return _config.capacity;
    }

    [[deprecated("I/O queue users should not track individual requests, but resources (weight, size) passing through the queue")]]
    size_t queued_requests() const {
        return _queued_requests;
    }

    // How many requests are sent to disk but not yet returned.
    [[deprecated("I/O queue users should not track individual requests, but resources (weight, size) passing through the queue")]]
    size_t requests_currently_executing() const {
        return _requests_executing;
    }

    void notify_requests_finished(fair_queue_ticket& desc);

    // Inform the underlying queue about the fact that some of our requests finished
    void process_completions();

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
    void rename_priority_class(io_priority_class pc, sstring new_name);

    friend class reactor;
private:
    config _config;
    static fair_queue::config make_fair_queue_config(config cfg);
};

}
