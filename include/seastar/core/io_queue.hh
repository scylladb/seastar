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

#include <boost/container/small_vector.hpp>
#include <seastar/core/sstring.hh>
#include <seastar/core/fair_queue.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/future.hh>
#include <seastar/core/internal/io_request.hh>
#include <mutex>
#include <array>

namespace seastar {

class io_priority_class;

[[deprecated("Use io_priority_class.rename")]]
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
using stream_id = unsigned;

class io_priority_class;
class io_desc_read_write;
class queued_io_request;
class io_group;

using io_group_ptr = std::shared_ptr<io_group>;

class io_queue {
public:
    class priority_class_data;

private:
    std::vector<std::unique_ptr<priority_class_data>> _priority_classes;
    io_group_ptr _group;
    boost::container::small_vector<fair_queue, 2> _streams;
    internal::io_sink& _sink;

    priority_class_data& find_or_create_class(const io_priority_class& pc);

    // The fields below are going away, they are just here so we can implement deprecated
    // functions that used to be provided by the fair_queue and are going away (from both
    // the fair_queue and the io_queue). Double-accounting for now will allow for easier
    // decoupling and is temporary
    size_t _queued_requests = 0;
    size_t _requests_executing = 0;
public:

    using clock_type = std::chrono::steady_clock;

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
        unsigned max_req_count = std::numeric_limits<int>::max();
        unsigned max_bytes_count = std::numeric_limits<int>::max();
        unsigned disk_req_write_to_read_multiplier = read_request_base_count;
        unsigned disk_bytes_write_to_read_multiplier = read_request_base_count;
        float disk_us_per_request = 0;
        float disk_us_per_byte = 0;
        size_t disk_read_saturation_length = std::numeric_limits<size_t>::max();
        size_t disk_write_saturation_length = std::numeric_limits<size_t>::max();
        sstring mountpoint = "undefined";
        bool duplex = false;
    };

    io_queue(io_group_ptr group, internal::io_sink& sink);
    ~io_queue();

    stream_id request_stream(internal::io_direction_and_length dnl) const noexcept;
    fair_queue_ticket request_fq_ticket(internal::io_direction_and_length dnl) const noexcept;

    future<size_t>
    queue_request(const io_priority_class& pc, size_t len, internal::io_request req, io_intent* intent) noexcept;
    void submit_request(io_desc_read_write* desc, internal::io_request req) noexcept;
    void cancel_request(queued_io_request& req) noexcept;
    void complete_cancelled_request(queued_io_request& req) noexcept;
    void complete_request(io_desc_read_write& desc) noexcept;


    [[deprecated("modern I/O queues should use a property file")]] size_t capacity() const;

    [[deprecated("I/O queue users should not track individual requests, but resources (weight, size) passing through the queue")]]
    size_t queued_requests() const {
        return _queued_requests;
    }

    // How many requests are sent to disk but not yet returned.
    [[deprecated("I/O queue users should not track individual requests, but resources (weight, size) passing through the queue")]]
    size_t requests_currently_executing() const {
        return _requests_executing;
    }

    // Dispatch requests that are pending in the I/O queue
    void poll_io_queue();

    clock_type::time_point next_pending_aio() const noexcept;

    sstring mountpoint() const;
    dev_t dev_id() const noexcept;

    future<> update_shares_for_class(io_priority_class pc, size_t new_shares);
    void rename_priority_class(io_priority_class pc, sstring new_name);

    struct request_limits {
        size_t max_read;
        size_t max_write;
    };

    request_limits get_request_limits() const noexcept;

private:
    static fair_queue::config make_fair_queue_config(const config& cfg);

    const config& get_config() const noexcept;
};

class io_group {
public:
    explicit io_group(io_queue::config io_cfg) noexcept;

private:
    friend class io_queue;
    const io_queue::config _config;
    std::vector<std::unique_ptr<fair_group>> _fgs;

    static fair_group::config make_fair_group_config(const io_queue::config& qcfg) noexcept;
};

inline const io_queue::config& io_queue::get_config() const noexcept {
    return _group->_config;
}

inline size_t io_queue::capacity() const {
    return get_config().capacity;
}

inline sstring io_queue::mountpoint() const {
    return get_config().mountpoint;
}

inline dev_t io_queue::dev_id() const noexcept {
    return get_config().devid;
}

}
