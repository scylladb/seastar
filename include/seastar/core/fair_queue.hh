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

#include <boost/intrusive/slist.hpp>
#include <seastar/core/sstring.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/shared_token_bucket.hh>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <fmt/ostream.h>

namespace bi = boost::intrusive;

namespace seastar {

/// \brief describes a request that passes through the \ref fair_queue.
///
/// A ticket is specified by a \c weight and a \c size. For example, one can specify a request of \c weight
/// 1 and \c size 16kB. If the \ref fair_queue accepts one such request per second, it will sustain 1 IOPS
/// at 16kB/s bandwidth.
///
/// \related fair_queue
class fair_queue_ticket {
    uint32_t _weight = 0; ///< the total weight of these requests for capacity purposes (IOPS).
    uint32_t _size = 0;        ///< the total effective size of these requests
public:
    /// Constructs a fair_queue_ticket with a given \c weight and a given \c size
    ///
    /// \param weight the weight of the request
    /// \param size the size of the request
    fair_queue_ticket(uint32_t weight, uint32_t size) noexcept;
    fair_queue_ticket() noexcept {}
    fair_queue_ticket operator+(fair_queue_ticket desc) const noexcept;
    fair_queue_ticket operator-(fair_queue_ticket desc) const noexcept;
    /// Increase the quantity represented in this ticket by the amount represented by \c desc
    /// \param desc another \ref fair_queue_ticket whose \c weight \c and size will be added to this one
    fair_queue_ticket& operator+=(fair_queue_ticket desc) noexcept;
    /// Decreases the quantity represented in this ticket by the amount represented by \c desc
    /// \param desc another \ref fair_queue_ticket whose \c weight \c and size will be decremented from this one
    fair_queue_ticket& operator-=(fair_queue_ticket desc) noexcept;
    /// Checks if the tickets fully equals to another one
    /// \param desc another \ref fair_queue_ticket to compare with
    bool operator==(const fair_queue_ticket& desc) const noexcept;

    /// \returns true if the fair_queue_ticket represents a non-zero quantity.
    ///
    /// For a fair_queue ticket to be non-zero, at least one of its represented quantities need to
    /// be non-zero
    explicit operator bool() const noexcept;
    bool is_non_zero() const noexcept;

    friend std::ostream& operator<<(std::ostream& os, fair_queue_ticket t);

    /// \returns the normalized value of this \ref fair_queue_ticket along a base axis
    ///
    /// The normalization function itself is an implementation detail, but one can expect either weight or
    /// size to have more or less relative importance depending on which of the dimensions in the
    /// denominator is relatively higher. For example, given this request a, and two other requests
    /// b and c, such that that c has the same \c weight but a higher \c size than b, one can expect
    /// the \c size component of this request to play a larger role.
    ///
    /// It is legal for the numerator to have one of the quantities set to zero, in which case only
    /// the other quantity is taken into consideration.
    ///
    /// It is however not legal for the axis to have any quantity set to zero.
    /// \param axis another \ref fair_queue_ticket to be used as a a base vector against which to normalize this fair_queue_ticket.
    float normalize(fair_queue_ticket axis) const noexcept;

    /*
     * For both dimentions checks if the first rover is ahead of the
     * second and returns the difference. If behind returns zero.
     */
    friend fair_queue_ticket wrapping_difference(const fair_queue_ticket& a, const fair_queue_ticket& b) noexcept;
};

/// \addtogroup io-module
/// @{

class fair_queue_entry {
public:
    // The capacity_t represents tokens each entry needs to get dispatched, in
    // a 'normalized' form -- converted from floating-point to fixed-point number
    // and scaled accrding to fair-group's token-bucket duration
    using capacity_t = uint64_t;
    friend class fair_queue;

private:
    capacity_t _capacity;
    bi::slist_member_hook<> _hook;

public:
    explicit fair_queue_entry(capacity_t c) noexcept
        : _capacity(c) {}
    using container_list_t = bi::slist<fair_queue_entry,
            bi::constant_time_size<false>,
            bi::cache_last<true>,
            bi::member_hook<fair_queue_entry, bi::slist_member_hook<>, &fair_queue_entry::_hook>>;

    capacity_t capacity() const noexcept { return _capacity; }
};

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
class fair_group {
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

    explicit fair_group(config cfg, unsigned nr_queues);
    fair_group(fair_group&&) = delete;

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

/// \brief Fair queuing class
///
/// This is a fair queue, allowing multiple request producers to queue requests
/// that will then be served proportionally to their classes' shares.
///
/// To each request, a weight can also be associated. A request of weight 1 will consume
/// 1 share. Higher weights for a request will consume a proportionally higher amount of
/// shares.
///
/// The user of this interface is expected to register multiple `priority_class_data`
/// objects, which will each have a shares attribute.
///
/// Internally, each priority class may keep a separate queue of requests.
/// Requests pertaining to a class can go through even if they are over its
/// share limit, provided that the other classes have empty queues.
///
/// When the classes that lag behind start seeing requests, the fair queue will serve
/// them first, until balance is restored. This balancing is expected to happen within
/// a certain time window that obeys an exponential decay.
class fair_queue {
public:
    /// \brief Fair Queue configuration structure.
    ///
    /// \sets the operation parameters of a \ref fair_queue
    /// \related fair_queue
    struct config {
        sstring label = "";
        std::chrono::microseconds tau = std::chrono::milliseconds(5);
    };

    using class_id = unsigned int;
    class priority_class_data;
    using capacity_t = fair_group::capacity_t;
    using signed_capacity_t = std::make_signed_t<capacity_t>;

private:
    using clock_type = std::chrono::steady_clock;
    using priority_class_ptr = priority_class_data*;
    struct class_compare {
        bool operator() (const priority_class_ptr& lhs, const priority_class_ptr & rhs) const noexcept;
    };

    class priority_queue : public std::priority_queue<priority_class_ptr, std::vector<priority_class_ptr>, class_compare> {
        using super = std::priority_queue<priority_class_ptr, std::vector<priority_class_ptr>, class_compare>;
    public:
        void reserve(size_t len) {
            c.reserve(len);
        }

        void assert_enough_capacity() const noexcept {
            SEASTAR_ASSERT(c.size() < c.capacity());
        }
    };

    config _config;
    fair_group& _group;
    clock_type::time_point _group_replenish;
    fair_queue_ticket _resources_executing;
    fair_queue_ticket _resources_queued;
    priority_queue _handles;
    std::vector<std::unique_ptr<priority_class_data>> _priority_classes;
    size_t _nr_classes = 0;
    capacity_t _last_accumulated = 0;

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

    // Total capacity of all requests waiting in the queue.
    capacity_t _queued_capacity = 0;

    void push_priority_class(priority_class_data& pc) noexcept;
    void push_priority_class_from_idle(priority_class_data& pc) noexcept;
    void pop_priority_class(priority_class_data& pc) noexcept;
    void plug_priority_class(priority_class_data& pc) noexcept;
    void unplug_priority_class(priority_class_data& pc) noexcept;

    // Replaces _pending with a new reservation starting at the current
    // group bucket tail.
    void grab_capacity(capacity_t cap) noexcept;
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
    reap_result reap_pending_capacity() noexcept;
public:
    /// Constructs a fair queue with configuration parameters \c cfg.
    ///
    /// \param cfg an instance of the class \ref config
    explicit fair_queue(fair_group& shared, config cfg);
    fair_queue(fair_queue&&) = delete;
    ~fair_queue();

    sstring label() const noexcept { return _config.label; }

    /// Registers a priority class against this fair queue.
    ///
    /// \param shares how many shares to create this class with
    void register_priority_class(class_id c, uint32_t shares);

    /// Unregister a priority class.
    ///
    /// It is illegal to unregister a priority class that still have pending requests.
    void unregister_priority_class(class_id c);

    void update_shares_for_class(class_id c, uint32_t new_shares);

    /// \return how much resources (weight, size) are currently queued for all classes.
    fair_queue_ticket resources_currently_waiting() const;

    /// \return the amount of resources (weight, size) currently executing
    fair_queue_ticket resources_currently_executing() const;

    capacity_t tokens_capacity(double tokens) const noexcept {
        return _group.tokens_capacity(tokens);
    }

    capacity_t maximum_capacity() const noexcept {
        return _group.maximum_capacity();
    }

    /// Queue the entry \c ent through this class' \ref fair_queue
    ///
    /// The user of this interface is supposed to call \ref notify_requests_finished when the
    /// request finishes executing - regardless of success or failure.
    void queue(class_id c, fair_queue_entry& ent) noexcept;

    void plug_class(class_id c) noexcept;
    void unplug_class(class_id c) noexcept;

    /// Notifies that ont request finished
    /// \param desc an instance of \c fair_queue_ticket structure describing the request that just finished.
    void notify_request_finished(fair_queue_entry::capacity_t cap) noexcept;
    void notify_request_cancelled(fair_queue_entry& ent) noexcept;

    /// Try to execute new requests if there is capacity left in the queue.
    void dispatch_requests(std::function<void(fair_queue_entry&)> cb);

    clock_type::time_point next_pending_aio() const noexcept;

    std::vector<seastar::metrics::impl::metric_definition_impl> metrics(class_id c);
};
/// @}

}

#if FMT_VERSION >= 90000
template <> struct fmt::formatter<seastar::fair_queue_ticket> : fmt::ostream_formatter {};
#endif
