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

#include "future.hh"
#include "semaphore.hh"
#include "shared_ptr.hh"
#include "print.hh"
#include "circular_buffer.hh"
#include "util/noncopyable_function.hh"
#include <queue>
#include <type_traits>
#include <experimental/optional>
#include <chrono>
#include <unordered_set>
#include <cmath>

namespace seastar {

/// \addtogroup io-module
/// @{

/// \cond internal
class priority_class {
    struct request {
        noncopyable_function<void()> func;
        unsigned weight;
    };
    friend class fair_queue;
    uint32_t _shares = 0;
    float _accumulated = 0;
    circular_buffer<request> _queue;
    bool _queued = false;

    friend struct shared_ptr_no_esft<priority_class>;
    explicit priority_class(uint32_t shares) : _shares(std::max(shares, 1u)) {}

    void update_shares(uint32_t shares) {
        _shares = (std::max(shares, 1u));
    }
public:
    /// \brief return the current amount of shares for this priority class
    uint32_t shares() const {
        return _shares;
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

/// \brief Fair queuing class
///
/// This is a fair queue, allowing multiple request producers to queue requests
/// that will then be served proportionally to their classes' shares.
///
/// To each request, a weight can also be associated. A request of weight 1 will consume
/// 1 share. Higher weights for a request will consume a proportionally higher amount of
/// shares.
///
/// The user of this interface is expected to register multiple \ref priority_class
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
    friend priority_class;

    struct class_compare {
        bool operator() (const priority_class_ptr& lhs, const priority_class_ptr& rhs) const {
            return lhs->_accumulated > rhs->_accumulated;
        }
    };

    unsigned _requests_executing = 0;
    unsigned _requests_queued = 0;
    unsigned _capacity;
    using clock_type = std::chrono::steady_clock::time_point;
    clock_type _base;
    std::chrono::microseconds _tau;
    using prioq = std::priority_queue<priority_class_ptr, std::vector<priority_class_ptr>, class_compare>;
    prioq _handles;
    std::unordered_set<priority_class_ptr> _all_classes;

    void push_priority_class(priority_class_ptr pc) {
        if (!pc->_queued) {
            _handles.push(pc);
            pc->_queued = true;
        }
    }

    priority_class_ptr pop_priority_class() {
        assert(!_handles.empty());
        auto h = _handles.top();
        _handles.pop();
        assert(h->_queued);
        h->_queued = false;
        return h;
    }

    float normalize_factor() const {
        return std::numeric_limits<float>::min();
    }

    void normalize_stats() {
        auto time_delta = std::log(normalize_factor()) * _tau;
        // time_delta is negative; and this may advance _base into the future
        _base -= std::chrono::duration_cast<clock_type::duration>(time_delta);
        for (auto& pc: _all_classes) {
            pc->_accumulated *= normalize_factor();
        }
    }
public:
    /// Constructs a fair queue with a given \c capacity.
    ///
    /// \param capacity how many concurrent requests are allowed in this queue.
    /// \param tau the queue exponential decay parameter, as in exp(-1/tau * t)
    explicit fair_queue(unsigned capacity, std::chrono::microseconds tau = std::chrono::milliseconds(100))
                                           : _capacity(capacity)
                                           , _base(std::chrono::steady_clock::now())
                                           , _tau(tau) {
    }

    /// Registers a priority class against this fair queue.
    ///
    /// \param shares, how many shares to create this class with
    priority_class_ptr register_priority_class(uint32_t shares) {
        priority_class_ptr pclass = make_lw_shared<priority_class>(shares);
        _all_classes.insert(pclass);
        return pclass;
    }

    /// Unregister a priority class.
    ///
    /// It is illegal to unregister a priority class that still have pending requests.
    void unregister_priority_class(priority_class_ptr pclass) {
        assert(pclass->_queue.empty());
        _all_classes.erase(pclass);
    }

    /// \return how many waiters are currently queued for all classes.
    size_t waiters() const {
        return _requests_queued;
    }

    /// \return the number of requests currently executing
    size_t requests_currently_executing() const {
        return _requests_executing;
    }

    /// Queue the function \c func through this class' \ref fair_queue, with weight \c weight
    ///
    /// It is expected that \c func doesn't throw. If it does throw, it will be just removed from
    /// the queue and discarded.
    ///
    /// The user of this interface is supposed to call \ref notify_requests_finished when the
    /// request finishes executing - regardless of success or failure.
    void queue(priority_class_ptr pc, unsigned weight, noncopyable_function<void()> func) {
        // We need to return a future in this function on which the caller can wait.
        // Since we don't know which queue we will use to execute the next request - if ours or
        // someone else's, we need a separate promise at this point.
        push_priority_class(pc);
        pc->_queue.push_back(priority_class::request{std::move(func), weight});
        _requests_queued++;
    }

    /// Notifies that \c finished requests finished
    void notify_requests_finished(unsigned finished) {
        _requests_executing -= finished;
    }

    /// Try to execute new requests if there is capacity left in the queue.
    void dispatch_requests() {
        while (_requests_queued && (_requests_executing < _capacity)) {
            priority_class_ptr h;
            do {
                h = pop_priority_class();
            } while (h->_queue.empty());

            auto req = std::move(h->_queue.front());
            h->_queue.pop_front();
            _requests_executing++;
            _requests_queued--;

            auto delta = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _base);
            auto req_cost  = float(req.weight) / h->_shares;
            auto cost  = expf(1.0f/_tau.count() * delta.count()) * req_cost;
            float next_accumulated = h->_accumulated + cost;
            while (std::isinf(next_accumulated)) {
                normalize_stats();
                // If we have renormalized, our time base will have changed. This should happen very infrequently
                delta = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _base);
                cost  = expf(1.0f/_tau.count() * delta.count()) * req_cost;
                next_accumulated = h->_accumulated + cost;
            }
            h->_accumulated = next_accumulated;

            if (!h->_queue.empty()) {
                push_priority_class(h);
            }
            req.func();
        }
    }

    /// Updates the current shares of this priority class
    ///
    /// \param new_shares the new number of shares for this priority class
    static void update_shares(priority_class_ptr pc, uint32_t new_shares) {
        pc->update_shares(new_shares);
    }
};
/// @}

}
