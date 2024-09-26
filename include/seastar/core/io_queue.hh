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

#ifndef SEASTAR_MODULE
#include <boost/container/static_vector.hpp>
#include <chrono>
#include <memory>
#include <vector>
#include <sys/uio.h>
#endif
#include <seastar/core/sstring.hh>
#include <seastar/core/fair_queue.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/future.hh>
#include <seastar/core/internal/io_request.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/util/spinlock.hh>
#include <seastar/util/modules.hh>

struct io_queue_for_tests;

namespace seastar {

class io_queue;
namespace internal {
const fair_group& get_fair_group(const io_queue& ioq, unsigned stream);
}

SEASTAR_MODULE_EXPORT
class io_intent;

namespace internal {
class io_sink;
}

using shard_id = unsigned;
using stream_id = unsigned;

class io_desc_read_write;
class queued_io_request;
class io_group;

using io_group_ptr = std::shared_ptr<io_group>;
using iovec_keeper = std::vector<::iovec>;

namespace internal {
struct maybe_priority_class_ref;
class priority_class {
    unsigned _id;
public:
    explicit priority_class(const scheduling_group& sg) noexcept;
    explicit priority_class(internal::maybe_priority_class_ref pc) noexcept;
    unsigned id() const noexcept { return _id; }
};
}

class io_queue {
public:
    class priority_class_data;

private:
    std::vector<std::unique_ptr<priority_class_data>> _priority_classes;
    io_group_ptr _group;
    boost::container::static_vector<fair_queue, 2> _streams;
    internal::io_sink& _sink;

    friend struct ::io_queue_for_tests;
    friend const fair_group& internal::get_fair_group(const io_queue& ioq, unsigned stream);

    priority_class_data& find_or_create_class(internal::priority_class pc);
    future<size_t> queue_request(internal::priority_class pc, internal::io_direction_and_length dnl, internal::io_request req, io_intent* intent, iovec_keeper iovs) noexcept;
    future<size_t> queue_one_request(internal::priority_class pc, internal::io_direction_and_length dnl, internal::io_request req, io_intent* intent, iovec_keeper iovs) noexcept;

    // The fields below are going away, they are just here so we can implement deprecated
    // functions that used to be provided by the fair_queue and are going away (from both
    // the fair_queue and the io_queue). Double-accounting for now will allow for easier
    // decoupling and is temporary
    size_t _queued_requests = 0;
    size_t _requests_executing = 0;
    uint64_t _requests_dispatched = 0;
    uint64_t _requests_completed = 0;

    // Flow monitor
    uint64_t _prev_dispatched = 0;
    uint64_t _prev_completed = 0;
    double _flow_ratio = 1.0;

    timer<lowres_clock> _averaging_decay_timer;

    const std::chrono::milliseconds _stall_threshold_min;
    std::chrono::milliseconds _stall_threshold;

    void update_flow_ratio() noexcept;
    void lower_stall_threshold() noexcept;

    metrics::metric_groups _metric_groups;
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
    static constexpr unsigned block_size_shift = 9;

    struct config {
        dev_t devid;
        unsigned long req_count_rate = std::numeric_limits<int>::max();
        unsigned long blocks_count_rate = std::numeric_limits<int>::max();
        unsigned disk_req_write_to_read_multiplier = read_request_base_count;
        unsigned disk_blocks_write_to_read_multiplier = read_request_base_count;
        size_t disk_read_saturation_length = std::numeric_limits<size_t>::max();
        size_t disk_write_saturation_length = std::numeric_limits<size_t>::max();
        sstring mountpoint = "undefined";
        bool duplex = false;
        std::chrono::duration<double> rate_limit_duration = std::chrono::milliseconds(1);
        size_t block_count_limit_min = 1;
        unsigned averaging_decay_ticks = 100;
        double flow_ratio_ema_factor = 0.95;
        double flow_ratio_backpressure_threshold = 1.1;
        std::chrono::milliseconds stall_threshold = std::chrono::milliseconds(100);
    };

    io_queue(io_group_ptr group, internal::io_sink& sink);
    ~io_queue();

    stream_id request_stream(internal::io_direction_and_length dnl) const noexcept;

    future<size_t> submit_io_read(internal::priority_class priority_class,
            size_t len, internal::io_request req, io_intent* intent, iovec_keeper iovs = {}) noexcept;
    future<size_t> submit_io_write(internal::priority_class priority_class,
            size_t len, internal::io_request req, io_intent* intent, iovec_keeper iovs = {}) noexcept;

    void submit_request(io_desc_read_write* desc, internal::io_request req) noexcept;
    void cancel_request(queued_io_request& req) noexcept;
    void complete_cancelled_request(queued_io_request& req) noexcept;
    void complete_request(io_desc_read_write& desc, std::chrono::duration<double> delay) noexcept;

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
    fair_queue_entry::capacity_t request_capacity(internal::io_direction_and_length dnl) const noexcept;

    sstring mountpoint() const;
    dev_t dev_id() const noexcept;

    void update_shares_for_class(internal::priority_class pc, size_t new_shares);
    future<> update_bandwidth_for_class(internal::priority_class pc, uint64_t new_bandwidth);
    void rename_priority_class(internal::priority_class pc, sstring new_name);
    void throttle_priority_class(const priority_class_data& pc) noexcept;
    void unthrottle_priority_class(const priority_class_data& pc) noexcept;

    struct request_limits {
        size_t max_read;
        size_t max_write;
    };

    request_limits get_request_limits() const noexcept;
    const config& get_config() const noexcept;

private:
    static fair_queue::config make_fair_queue_config(const config& cfg, sstring label);
    void register_stats(sstring name, priority_class_data& pc);
};

class io_group {
public:
    explicit io_group(io_queue::config io_cfg, unsigned nr_queues);
    ~io_group();
    struct priority_class_data;

    std::chrono::duration<double> io_latency_goal() const noexcept;

private:
    friend class io_queue;
    friend struct ::io_queue_for_tests;
    friend const fair_group& internal::get_fair_group(const io_queue& ioq, unsigned stream);

    const io_queue::config _config;
    size_t _max_request_length[2];
    boost::container::static_vector<fair_group, 2> _fgs;
    std::vector<std::unique_ptr<priority_class_data>> _priority_classes;
    util::spinlock _lock;
    const shard_id _allocated_on;

    static fair_group::config make_fair_group_config(const io_queue::config& qcfg) noexcept;
    priority_class_data& find_or_create_class(internal::priority_class pc);
};

inline const io_queue::config& io_queue::get_config() const noexcept {
    return _group->_config;
}

inline sstring io_queue::mountpoint() const {
    return get_config().mountpoint;
}

inline dev_t io_queue::dev_id() const noexcept {
    return get_config().devid;
}

namespace internal {
double request_tokens(io_direction_and_length dnl, const io_queue::config& cfg) noexcept;
}

}
