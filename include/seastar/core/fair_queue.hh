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
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/circular_buffer.hh>
#include <atomic>
#include <queue>
#include <chrono>
#include <unordered_set>
#include <optional>

namespace bi = boost::intrusive;

namespace seastar {

class fair_group_rover;

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
    friend class fair_group_rover;
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

    std::chrono::microseconds duration_at_pace(float weight_pace, float size_pace) const noexcept;

    /// \returns true if the fair_queue_ticket represents a non-zero quantity.
    ///
    /// For a fair_queue ticket to be non-zero, at least one of its represented quantities need to
    /// be non-zero
    explicit operator bool() const noexcept;

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
};

class fair_group_rover {
    uint32_t _weight = 0;
    uint32_t _size = 0;

public:
    fair_group_rover(uint32_t weight, uint32_t size) noexcept;

    /*
     * For both dimentions checks if the current rover is ahead of the
     * other and returns the difference. If this is behind returns zero.
     */
    fair_queue_ticket maybe_ahead_of(const fair_group_rover& other) const noexcept;
    fair_group_rover operator+(fair_queue_ticket t) const noexcept;
    fair_group_rover& operator+=(fair_queue_ticket t) noexcept;

    friend std::ostream& operator<<(std::ostream& os, fair_group_rover r);
};

/// \addtogroup io-module
/// @{

class fair_queue_entry {
    friend class fair_queue;

    fair_queue_ticket _ticket;
    bi::slist_member_hook<> _hook;

public:
    fair_queue_entry(fair_queue_ticket t) noexcept
        : _ticket(std::move(t)) {}
    using container_list_t = bi::slist<fair_queue_entry,
            bi::constant_time_size<false>,
            bi::cache_last<true>,
            bi::member_hook<fair_queue_entry, bi::slist_member_hook<>, &fair_queue_entry::_hook>>;

    fair_queue_ticket ticket() const noexcept { return _ticket; }
};

/// \cond internal
class priority_class {
    friend class fair_queue;
    uint32_t _shares = 0;
    float _accumulated = 0;
    fair_queue_entry::container_list_t _queue;
    bool _queued = false;

    friend struct shared_ptr_no_esft<priority_class>;
    explicit priority_class(uint32_t shares) noexcept : _shares(std::max(shares, 1u)) {}

public:
    /// \brief return the current amount of shares for this priority class
    uint32_t shares() const noexcept {
        return _shares;
    }

    void update_shares(uint32_t shares) noexcept {
        _shares = (std::max(shares, 1u));
    }
};
/// \endcond

/// \brief Priority class, to be used with a given \ref fair_queue
///
/// An instance of this class is associated with a given \ref fair_queue. When registering
/// a class, the caller will receive a \ref lw_shared_ptr to an object of this class. All its methods
/// are private, so the only thing the caller is expected to do with it is to pass it later
/// to the \ref fair_queue to identify a given class.
///
/// \related fair_queue
using priority_class_ptr = lw_shared_ptr<priority_class>;

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
    using fair_group_atomic_rover = std::atomic<fair_group_rover>;
    static_assert(fair_group_atomic_rover::is_always_lock_free);

    fair_group_atomic_rover _capacity_tail;
    fair_group_atomic_rover _capacity_head;
    fair_queue_ticket _maximum_capacity;

public:
    struct config {
        unsigned max_req_count;
        unsigned max_bytes_count;

        /// Constructs a config with the given \c capacity, expressed in maximum
        /// values for requests and bytes.
        ///
        /// \param max_requests how many concurrent requests are allowed in this queue.
        /// \param max_bytes how many total bytes are allowed in this queue.
        config(unsigned max_requests, unsigned max_bytes) noexcept
                : max_req_count(max_requests), max_bytes_count(max_bytes) {}
    };
    explicit fair_group(config cfg) noexcept;

    fair_queue_ticket maximum_capacity() const noexcept { return _maximum_capacity; }
    fair_group_rover grab_capacity(fair_queue_ticket cap) noexcept;
    void release_capacity(fair_queue_ticket cap) noexcept;

    fair_group_rover head() const noexcept {
        return _capacity_head.load(std::memory_order_relaxed);
    }
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
/// The user of this interface is expected to register multiple `priority_class`
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
        std::chrono::microseconds tau = std::chrono::milliseconds(100);
        // Time (in microseconds) is takes to process one ticket value
        float ticket_size_pace;
        float ticket_weight_pace;
    };
private:
    struct class_compare {
        bool operator() (const priority_class_ptr& lhs, const priority_class_ptr& rhs) const {
            return lhs->_accumulated > rhs->_accumulated;
        }
    };

    config _config;
    fair_group& _group;
    fair_queue_ticket _resources_executing;
    fair_queue_ticket _resources_queued;
    unsigned _requests_executing = 0;
    unsigned _requests_queued = 0;
    using clock_type = std::chrono::steady_clock::time_point;
    clock_type _base;
    using prioq = std::priority_queue<priority_class_ptr, std::vector<priority_class_ptr>, class_compare>;
    prioq _handles;
    std::unordered_set<priority_class_ptr> _all_classes;

    /*
     * When the shared capacity os over the local queue delays
     * further dispatching untill better times
     *
     * \orig_tail  -- the value group tail rover had when it happened
     * \cap   -- the capacity that's accounted on the group
     *
     * The last field is needed to "rearm" the wait in case
     * queue decides that it wants to dispatch another capacity
     * in the middle of the waiting
     */
    struct pending {
        fair_group_rover orig_tail;
        fair_queue_ticket cap;

        pending(fair_group_rover t, fair_queue_ticket c) noexcept : orig_tail(t), cap(c) {}
    };

    std::optional<pending> _pending;

    priority_class_ptr peek_priority_class();
    void push_priority_class(priority_class_ptr pc);
    void pop_priority_class(priority_class_ptr pc);

    float normalize_factor() const;

    void normalize_stats();

    // Estimated time to process the given ticket
    std::chrono::microseconds duration(fair_queue_ticket desc) const noexcept {
        return desc.duration_at_pace(_config.ticket_weight_pace, _config.ticket_size_pace);
    }

    bool grab_capacity(fair_queue_ticket cap) noexcept;
    bool grab_pending_capacity(fair_queue_ticket cap) noexcept;
public:
    /// Constructs a fair queue with configuration parameters \c cfg.
    ///
    /// \param cfg an instance of the class \ref config
    explicit fair_queue(fair_group& shared, config cfg);

    /// Registers a priority class against this fair queue.
    ///
    /// \param shares how many shares to create this class with
    priority_class_ptr register_priority_class(uint32_t shares);

    /// Unregister a priority class.
    ///
    /// It is illegal to unregister a priority class that still have pending requests.
    void unregister_priority_class(priority_class_ptr pclass);

    /// \return how many waiters are currently queued for all classes.
    [[deprecated("fair_queue users should not track individual requests, but resources (weight, size) passing through the queue")]]
    size_t waiters() const;

    /// \return the number of requests currently executing
    [[deprecated("fair_queue users should not track individual requests, but resources (weight, size) passing through the queue")]]
    size_t requests_currently_executing() const;

    /// \return how much resources (weight, size) are currently queued for all classes.
    fair_queue_ticket resources_currently_waiting() const;

    /// \return the amount of resources (weight, size) currently executing
    fair_queue_ticket resources_currently_executing() const;

    /// Queue the entry \c ent through this class' \ref fair_queue
    ///
    /// The user of this interface is supposed to call \ref notify_requests_finished when the
    /// request finishes executing - regardless of success or failure.
    void queue(priority_class_ptr pc, fair_queue_entry& ent);

    /// Notifies that ont request finished
    /// \param desc an instance of \c fair_queue_ticket structure describing the request that just finished.
    void notify_requests_finished(fair_queue_ticket desc, unsigned nr = 1) noexcept;
    void notify_request_cancelled(fair_queue_entry& ent) noexcept;

    /// Try to execute new requests if there is capacity left in the queue.
    void dispatch_requests(std::function<void(fair_queue_entry&)> cb);

    clock_type next_pending_aio() const noexcept {
        if (_pending) {
            /*
             * We expect the disk to release the ticket within some time,
             * but it's ... OK if it doesn't -- the pending wait still
             * needs the head rover value to be ahead of the needed value.
             *
             * It may happen that the capacity gets released before we think
             * it will, in this case we will wait for the full value again,
             * which's sub-optimal. The expectation is that we think disk
             * works faster, than it really does.
             */
            fair_group_rover pending_head = _pending->orig_tail + _pending->cap;
            fair_queue_ticket over = pending_head.maybe_ahead_of(_group.head());
            return std::chrono::steady_clock::now() + duration(over);
        }

        return std::chrono::steady_clock::time_point::max();
    }
};
/// @}

}
