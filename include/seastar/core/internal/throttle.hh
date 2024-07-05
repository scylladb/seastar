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
 * Copyright (C) 2016 ScyllaDB
 */
#pragma once

#include <seastar/core/sstring.hh>
#include <seastar/util/shared_token_bucket.hh>

namespace seastar {

/// \brief Group of queues class
///
/// This is a fair group. It's attached by one or mode fair queues. On machines having the
/// big* amount of shards, queues use the group to borrow/lend the needed capacity for
/// requests dispatching.
///
/// * Big means that when all shards sumbit requests alltogether the disk is unable to
/// dispatch them efficiently. The inability can be of two kinds -- either disk cannot
/// cope with the number of arriving requests, or the total size of the data withing
/// the given time frame exceeds the disk throughput.
class shared_throttle {
public:
    using capacity_t = uint64_t; // XXX -- this could be a template parameter
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

    explicit shared_throttle(config cfg, unsigned nr_queues);
    shared_throttle(shared_throttle&&) = delete;

    capacity_t maximum_capacity() const noexcept { return _token_bucket.limit(); }
    capacity_t per_tick_grab_threshold() const noexcept { return _per_tick_threshold; }
    capacity_t grab_capacity(capacity_t cap) noexcept;
    clock_type::time_point replenished_ts() const noexcept { return _token_bucket.replenished_ts(); }
    void replenish_capacity(clock_type::time_point now) noexcept;
    void maybe_replenish_capacity(clock_type::time_point& local_ts) noexcept;

    capacity_t capacity_deficiency(capacity_t from) const noexcept;

    std::chrono::duration<double> rate_limit_duration() const noexcept {
        std::chrono::duration<double, rate_resolution> dur((double)_token_bucket.limit() / _token_bucket.rate());
        return std::chrono::duration_cast<std::chrono::duration<double>>(dur);
    }

    const token_bucket_t& token_bucket() const noexcept { return _token_bucket; }
};

class throttle {
    using clock_type = std::chrono::steady_clock;

    /*
     * When the shared capacity os over the local queue delays
     * further dispatching untill better times
     *
     * \head  -- the value group head rover is expected to cross
     * \cap   -- the capacity that's accounted on the group
     *
     * The last field is needed to "rearm" the wait in case
     * queue decides that it wants to dispatch another capacity
     * in the middle of the waiting
     */
    struct pending {
        shared_throttle::capacity_t head;
        shared_throttle::capacity_t cap;

        pending(shared_throttle::capacity_t t, shared_throttle::capacity_t c) noexcept : head(t), cap(c) {}
    };

    shared_throttle& _group;
    clock_type::time_point _group_replenish;
    std::optional<pending> _pending;
public:
    throttle(shared_throttle& st) noexcept
        : _group(st)
        , _group_replenish(clock_type::now())
    {}

    enum class grab_result { grabbed, cant_preempt, pending };
    grab_result grab_capacity(shared_throttle::capacity_t) noexcept;
    grab_result grab_pending_capacity(shared_throttle::capacity_t) noexcept;

    shared_throttle::capacity_t tokens_capacity(double tokens) const noexcept {
        return _group.tokens_capacity(tokens);
    }

    shared_throttle::capacity_t maximum_capacity() const noexcept {
        return _group.maximum_capacity();
    }

    shared_throttle::capacity_t per_tick_grab_threshold() const noexcept {
        return _group.per_tick_grab_threshold();
    }

    clock_type::time_point next_pending() const noexcept;
};

} // seastar namespace
