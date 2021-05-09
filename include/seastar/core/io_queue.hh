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
#include <seastar/core/future.hh>
#include <seastar/core/internal/io_request.hh>
#include <mutex>
#include <array>

namespace seastar {

class io_priority_class;

/// Renames an io priority class
///
/// Renames an `io_priority_class` previously created with register_one_priority_class().
///
/// The operation is global and affects all shards.
/// The operation affects the exported statistics labels.
///
/// \param pc The io priority class to be renamed
/// \param new_name The new name for the io priority class
/// \return a future that is ready when the io priority class have been renamed
future<>
rename_priority_class(io_priority_class pc, sstring new_name);

class io_intent;

namespace internal {
class io_sink;
namespace linux_abi {

struct io_event;
struct iocb;

}
}

using shard_id = unsigned;

class io_priority_class;
class io_queue;
class io_desc_read_write;
class queued_io_request;

class io_group {
public:
    struct config {
        unsigned max_req_count = std::numeric_limits<int>::max();
        unsigned max_bytes_count = std::numeric_limits<int>::max();
        unsigned disk_req_write_to_read_multiplier;
    };
    explicit io_group(config cfg) noexcept;

private:
    friend class io_queue;
    fair_group _fg;
    const unsigned _max_bytes_count;

    static fair_group::config make_fair_group_config(config cfg) noexcept;
};

using io_group_ptr = std::shared_ptr<io_group>;
class priority_class_data;

class io_queue {
private:
    std::vector<std::unique_ptr<priority_class_data>> _priority_classes;
    io_group_ptr _group;
    fair_queue _fq;
    internal::io_sink& _sink;

    static constexpr unsigned _max_classes = 2048;
    static std::mutex _register_lock;
    static std::array<uint32_t, _max_classes> _registered_shares;
    static std::array<sstring, _max_classes> _registered_names;

public:
    static io_priority_class register_one_priority_class(sstring name, uint32_t shares);
    static bool rename_one_priority_class(io_priority_class pc, sstring name);

private:
    priority_class_data& find_or_create_class(const io_priority_class& pc);

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
    static constexpr unsigned request_ticket_size_shift = 9;
    static constexpr unsigned minimal_request_size = 512;

    struct config {
        dev_t devid;
        unsigned capacity = std::numeric_limits<unsigned>::max();
        unsigned disk_req_write_to_read_multiplier = read_request_base_count;
        unsigned disk_bytes_write_to_read_multiplier = read_request_base_count;
        float disk_us_per_request = 0;
        float disk_us_per_byte = 0;
        size_t disk_read_saturation_length = std::numeric_limits<size_t>::max();
        size_t disk_write_saturation_length = std::numeric_limits<size_t>::max();
        sstring mountpoint = "undefined";
    };

    io_queue(io_group_ptr group, internal::io_sink& sink, config cfg);
    ~io_queue();

    fair_queue_ticket request_fq_ticket(const internal::io_request& req, size_t len) const;

    future<size_t>
    queue_request(const io_priority_class& pc, size_t len, internal::io_request req, io_intent* intent) noexcept;
    void submit_request(io_desc_read_write* desc, internal::io_request req) noexcept;
    void cancel_request(queued_io_request& req) noexcept;
    void complete_cancelled_request(queued_io_request& req) noexcept;

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

    void notify_requests_finished(fair_queue_ticket& desc) noexcept;

    // Dispatch requests that are pending in the I/O queue
    void poll_io_queue();

    std::chrono::steady_clock::time_point next_pending_aio() const noexcept {
        return _fq.next_pending_aio();
    }

    sstring mountpoint() const {
        return _config.mountpoint;
    }

    dev_t dev_id() const noexcept {
        return _config.devid;
    }

    future<> update_shares_for_class(io_priority_class pc, size_t new_shares);
    void rename_priority_class(io_priority_class pc, sstring new_name);

    struct request_limits {
        size_t max_read;
        size_t max_write;
    };

    request_limits get_request_limits() const noexcept;

private:
    config _config;
    static fair_queue::config make_fair_queue_config(config cfg);
};

}
