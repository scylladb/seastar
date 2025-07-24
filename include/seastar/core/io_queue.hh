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
#include <seastar/util/shared_token_bucket.hh>

struct io_queue_for_tests;

namespace seastar {

class io_queue;
class io_throttler;

namespace internal {
const io_throttler& get_throttler(const io_queue& ioq, unsigned stream);
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
class priority_class {
    unsigned _id;
public:
    explicit priority_class(const scheduling_group& sg) noexcept;
    unsigned id() const noexcept { return _id; }
};
}

class io_queue {
public:
    class priority_class_data;
    using clock_type = std::chrono::steady_clock;

private:
    std::vector<std::unique_ptr<priority_class_data>> _priority_classes;
    io_group_ptr _group;
    const unsigned _id;
    struct stream {
        using capacity_t = fair_queue_entry::capacity_t;
        fair_queue fq;
        clock_type::time_point replenish;
        io_throttler& out;
        // _pending represents a reservation of tokens from the bucket.
        //
        // In the "dispatch timeline" defined by the growing bucket head of the group,
        // tokens in the range [_pending.head - cap, _pending.head) belong
        // to this queue.
        //
        // For example, if:
        //    _group._token_bucket.head == 300
        //    _pending.head == 700
        //    _pending.cap == 500
        // then the reservation is [200, 700), 100 tokens are ready to be dispatched by this queue,
        // and another 400 tokens are going to be appear soon. (And after that, this queue
        // will be able to make its next reservation).
        struct pending {
            capacity_t head = 0;
            capacity_t cap = 0;
        };
        pending _pending;
        stream(io_throttler& t, fair_queue::config cfg)
            : fq(std::move(cfg))
            , replenish(clock_type::now())
            , out(t)
        {}

        // Shaves off the fulfilled frontal part from `_pending` (if any),
        // and returns the fulfilled tokens in `ready_tokens`.
        // Sets `our_turn_has_come` to the truth value of "`_pending` is empty or
        // there are no unfulfilled reservations (from other shards) earlier than `_pending`".
        //
        // Assumes that `_group.maybe_replenish_capacity()` was called recently.
        struct reap_result {
            capacity_t ready_tokens;
            bool our_turn_has_come;
        };
        enum class grab_result { ok, stop, again };

        clock_type::time_point next_pending_aio() const noexcept;
        reap_result reap_pending_capacity() noexcept;
        grab_result grab_capacity(capacity_t cap, reap_result& available);

        std::vector<seastar::metrics::impl::metric_definition_impl> metrics(const priority_class_data&);
    };
    boost::container::static_vector<stream, 2> _streams;
    internal::io_sink& _sink;

    friend struct ::io_queue_for_tests;
    friend const io_throttler& internal::get_throttler(const io_queue& ioq, unsigned stream);

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
        unsigned id;
        unsigned long req_count_rate = std::numeric_limits<unsigned long>::max();
        unsigned long blocks_count_rate = std::numeric_limits<unsigned long>::max();
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
        std::chrono::microseconds tau = std::chrono::milliseconds(5);
        bool max_cost_function = true;
    };

    io_queue(io_group_ptr group, internal::io_sink& sink);
    ~io_queue();

    stream_id request_stream(internal::io_direction_and_length dnl) const noexcept;

    future<size_t> submit_io_read(size_t len, internal::io_request req, io_intent* intent, iovec_keeper iovs = {}) noexcept;
    future<size_t> submit_io_write(size_t len, internal::io_request req, io_intent* intent, iovec_keeper iovs = {}) noexcept;

    void submit_request(io_desc_read_write* desc, internal::io_request req) noexcept;
    void cancel_request(queued_io_request& req) noexcept;
    void complete_cancelled_request(queued_io_request& req) noexcept;
    void complete_request(io_desc_read_write& desc, std::chrono::duration<double> delay) noexcept;

    // Dispatch requests that are pending in the I/O queue
    void poll_io_queue();

    clock_type::time_point next_pending_aio() const noexcept;
    fair_queue_entry::capacity_t request_capacity(internal::io_direction_and_length dnl) const noexcept;

    sstring mountpoint() const;
    unsigned id() const noexcept { return _id; }

    void update_shares_for_class(internal::priority_class pc, size_t new_shares);
    void update_shares_for_class_group(unsigned index, size_t new_shares);
    future<> update_bandwidth_for_class(internal::priority_class pc, uint64_t new_bandwidth);
    void rename_priority_class(internal::priority_class pc, sstring new_name);
    void destroy_priority_class(internal::priority_class pc) noexcept;
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

/// \brief Outgoing throttler
///
/// This is a fair group. It's attached by one or mode fair queues. On machines having the
/// big* amount of shards, queues use the group to borrow/lend the needed capacity for
/// requests dispatching.
///
/// * Big means that when all shards sumbit requests alltogether the disk is unable to
/// dispatch them efficiently. The inability can be of two kinds -- either disk cannot
/// cope with the number of arriving requests, or the total size of the data withing
/// the given time frame exceeds the disk throughput.
class io_throttler {
public:
    using capacity_t = fair_queue_entry::capacity_t;
    using clock_type = std::chrono::steady_clock;

    /*
     * tldr; The math
     *
     *    Bw, Br -- write/read bandwidth (bytes per second)
     *    Ow, Or -- write/read iops (ops per second)
     *
     *    xx_max -- their maximum values (configured)
     *
     * Throttling formula:
     *
     *    Bw/Bw_max + Br/Br_max + Ow/Ow_max + Or/Or_max <= K
     *
     * where K is the scalar value <= 1.0 (also configured)
     *
     * Bandwidth is bytes time derivatite, iops is ops time derivative, i.e.
     * Bx = d(bx)/dt, Ox = d(ox)/dt. Then the formula turns into
     *
     *   d(bw/Bw_max + br/Br_max + ow/Ow_max + or/Or_max)/dt <= K
     *
     * Fair queue tickets are {w, s} weight-size pairs that are
     *
     *   s = read_base_count * br, for reads
     *       Br_max/Bw_max * read_base_count * bw, for writes
     *
     *   w = read_base_count, for reads
     *       Or_max/Ow_max * read_base_count, for writes
     *
     * Thus the formula turns into
     *
     *   d(sum(w/W + s/S))/dr <= K
     *
     * where {w, s} is the ticket value if a request and sum summarizes the
     * ticket values from all the requests seen so far, {W, S} is the ticket
     * value that corresonds to a virtual summary of Or_max requests of
     * Br_max size total.
     */

    /*
     * The normalization results in a float of the 2^-30 seconds order of
     * magnitude. Not to invent float point atomic arithmetics, the result
     * is converted to an integer by multiplying by a factor that's large
     * enough to turn these values into a non-zero integer.
     *
     * Also, the rates in bytes/sec when adjusted by io-queue according to
     * multipliers become too large to be stored in 32-bit ticket value.
     * Thus the rate resolution is applied. The t.bucket is configured with a
     * time period for which the speeds from F (in above formula) are taken.
     */

    static constexpr float fixed_point_factor = float(1 << 24);
    using rate_resolution = std::milli;
    using token_bucket_t = internal::shared_token_bucket<capacity_t, rate_resolution, internal::capped_release::no>;

private:

    /*
     * The dF/dt <= K limitation is managed by the modified token bucket
     * algo where tokens are ticket.normalize(cost_capacity), the refill
     * rate is K.
     *
     * The token bucket algo must have the limit on the number of tokens
     * accumulated. Here it's configured so that it accumulates for the
     * latency_goal duration.
     *
     * The replenish threshold is the minimal number of tokens to put back.
     * It's reserved for future use to reduce the load on the replenish
     * timestamp.
     *
     * The timestamp, in turn, is the time when the bucket was replenished
     * last. Every time a shard tries to get tokens from bucket it first
     * tries to convert the time that had passed since this timestamp
     * into more tokens in the bucket.
     */

    token_bucket_t _token_bucket;
    const capacity_t _per_tick_threshold;

public:

    // Convert internal capacity value back into the real token
    static double capacity_tokens(capacity_t cap) noexcept {
        return (double)cap / fixed_point_factor / token_bucket_t::rate_cast(std::chrono::seconds(1)).count();
    }

    // Convert floating-point tokens into the token bucket capacity
    static capacity_t tokens_capacity(double tokens) noexcept {
        return tokens * token_bucket_t::rate_cast(std::chrono::seconds(1)).count() * fixed_point_factor;
    }

    auto capacity_duration(capacity_t cap) const noexcept {
        return _token_bucket.duration_for(cap);
    }

    struct config {
        sstring label = "";
        /*
         * There are two "min" values that can be configured. The former one
         * is the minimal weight:size pair that the upper layer is going to
         * submit. However, it can submit _larger_ values, and the fair queue
         * must accept those as large as the latter pair (but it can accept
         * even larger values, of course)
         */
        double min_tokens = 0.0;
        double limit_min_tokens = 0.0;
        std::chrono::duration<double> rate_limit_duration = std::chrono::milliseconds(1);
    };

    explicit io_throttler(config cfg, unsigned nr_queues);
    io_throttler(io_throttler&&) = delete;

    capacity_t maximum_capacity() const noexcept { return _token_bucket.limit(); }
    capacity_t per_tick_grab_threshold() const noexcept { return _per_tick_threshold; }
    capacity_t grab_capacity(capacity_t cap) noexcept;
    clock_type::time_point replenished_ts() const noexcept { return _token_bucket.replenished_ts(); }
    void refund_tokens(capacity_t) noexcept;
    void replenish_capacity(clock_type::time_point now) noexcept;
    void maybe_replenish_capacity(clock_type::time_point& local_ts) noexcept;

    capacity_t capacity_deficiency(capacity_t from) const noexcept;

    std::chrono::duration<double> rate_limit_duration() const noexcept {
        std::chrono::duration<double, rate_resolution> dur((double)_token_bucket.limit() / _token_bucket.rate());
        return std::chrono::duration_cast<std::chrono::duration<double>>(dur);
    }

    const token_bucket_t& token_bucket() const noexcept { return _token_bucket; }
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
    friend const io_throttler& internal::get_throttler(const io_queue& ioq, unsigned stream);

    /*
     * This value is used as a cut-off point for calculating the maximum request length.
     * We look for max(2^i) such that capacity(2^i) < maximum capacity. If 2^i gets bigger
     * than this value, we stop looking and the maximum request length is set to this value.
     */
    static constexpr unsigned request_length_limit = 16 << 20; // 16 MiB

    const io_queue::config _config;
    size_t _max_request_length[2] = {
        request_length_limit, // write
        request_length_limit  // read
    };
    boost::container::static_vector<io_throttler, 2> _fgs;
    std::vector<std::unique_ptr<priority_class_data>> _priority_classes;
    util::spinlock _lock;
    const shard_id _allocated_on;

    static io_throttler::config configure_throttler(const io_queue::config& qcfg) noexcept;
    priority_class_data& find_or_create_class(internal::priority_class pc);

    inline size_t max_request_length(int dnl_idx) const noexcept {
        return _max_request_length[dnl_idx];
    }
};

inline const io_queue::config& io_queue::get_config() const noexcept {
    return _group->_config;
}

inline sstring io_queue::mountpoint() const {
    return get_config().mountpoint;
}

namespace internal {
double request_tokens(io_direction_and_length dnl, const io_queue::config& cfg) noexcept;
}

}
