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

#include <boost/intrusive/parent_from_member.hpp>
#include <seastar/core/fair_queue.hh>
#include <seastar/core/future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/core/reactor.hh>
#include <queue>
#include <chrono>
#include <unordered_set>
#include <cmath>

#include "fmt/format.h"
#include "fmt/ostream.h"

namespace seastar {

static_assert(sizeof(fair_queue_ticket) == sizeof(uint64_t), "unexpected fair_queue_ticket size");
static_assert(sizeof(fair_queue_entry) <= 3 * sizeof(void*), "unexpected fair_queue_entry::_hook size");
static_assert(sizeof(fair_queue_entry::container_list_t) == 2 * sizeof(void*), "unexpected priority_class::_queue size");

fair_queue_ticket::fair_queue_ticket(uint32_t weight, uint32_t size) noexcept
    : _weight(weight)
    , _size(size)
{}

float fair_queue_ticket::normalize(fair_queue_ticket denominator) const noexcept {
    return float(_weight) / denominator._weight + float(_size) / denominator._size;
}

fair_queue_ticket fair_queue_ticket::operator+(fair_queue_ticket desc) const noexcept {
    return fair_queue_ticket(_weight + desc._weight, _size + desc._size);
}

fair_queue_ticket& fair_queue_ticket::operator+=(fair_queue_ticket desc) noexcept {
    _weight += desc._weight;
    _size += desc._size;
    return *this;
}

fair_queue_ticket fair_queue_ticket::operator-(fair_queue_ticket desc) const noexcept {
    return fair_queue_ticket(_weight - desc._weight, _size - desc._size);
}

fair_queue_ticket& fair_queue_ticket::operator-=(fair_queue_ticket desc) noexcept {
    _weight -= desc._weight;
    _size -= desc._size;
    return *this;
}

fair_queue_ticket::operator bool() const noexcept {
    return (_weight > 0) || (_size > 0);
}

bool fair_queue_ticket::operator==(const fair_queue_ticket& o) const noexcept {
    return _weight == o._weight && _size == o._size;
}

std::ostream& operator<<(std::ostream& os, fair_queue_ticket t) {
    return os << t._weight << ":" << t._size;
}

fair_group_rover::fair_group_rover(uint32_t weight, uint32_t size) noexcept
        : _weight(weight)
        , _size(size)
{}

fair_queue_ticket fair_group_rover::maybe_ahead_of(const fair_group_rover& other) const noexcept {
    return fair_queue_ticket(std::max<int32_t>(_weight - other._weight, 0),
            std::max<int32_t>(_size - other._size, 0));
}

fair_group_rover fair_group_rover::operator+(fair_queue_ticket t) const noexcept {
    return fair_group_rover(_weight + t._weight, _size + t._size);
}

fair_group_rover& fair_group_rover::operator+=(fair_queue_ticket t) noexcept {
    _weight += t._weight;
    _size += t._size;
    return *this;
}

std::ostream& operator<<(std::ostream& os, fair_group_rover r) {
    return os << r._weight << ":" << r._size;
}

fair_group::fair_group(config cfg) noexcept
        : _capacity_tail(fair_group_rover(0, 0))
        , _capacity_head(fair_group_rover(cfg.max_req_count, cfg.max_bytes_count))
        , _maximum_capacity(cfg.max_req_count, cfg.max_bytes_count)
{
    assert(!_capacity_tail.load(std::memory_order_relaxed)
                .maybe_ahead_of(_capacity_head.load(std::memory_order_relaxed)));
    seastar_logger.debug("Created fair group, capacity {}:{}", cfg.max_req_count, cfg.max_bytes_count);
}

fair_group_rover fair_group::grab_capacity(fair_queue_ticket cap) noexcept {
    fair_group_rover cur = _capacity_tail.load(std::memory_order_relaxed);
    while (!_capacity_tail.compare_exchange_weak(cur, cur + cap)) ;
    return cur;
}

void fair_group::release_capacity(fair_queue_ticket cap) noexcept {
    fair_group_rover cur = _capacity_head.load(std::memory_order_relaxed);
    while (!_capacity_head.compare_exchange_weak(cur, cur + cap)) ;
}

fair_queue::fair_queue(fair_group& group, config cfg)
    : _config(std::move(cfg))
    , _group(group)
    , _base(std::chrono::steady_clock::now())
{
    seastar_logger.debug("Created fair queue, ticket pace {}:{}", cfg.ticket_weight_pace, cfg.ticket_size_pace);
}

void fair_queue::push_priority_class(priority_class_ptr pc) {
    if (!pc->_queued) {
        _handles.push(pc);
        pc->_queued = true;
    }
}

priority_class_ptr fair_queue::peek_priority_class() {
    assert(!_handles.empty());
    return _handles.top();
}

void fair_queue::pop_priority_class(priority_class_ptr pc) {
    assert(pc->_queued);
    pc->_queued = false;
    _handles.pop();
}

float fair_queue::normalize_factor() const {
    return std::numeric_limits<float>::min();
}

void fair_queue::normalize_stats() {
    auto time_delta = std::log(normalize_factor()) * _config.tau;
    // time_delta is negative; and this may advance _base into the future
    _base -= std::chrono::duration_cast<clock_type::duration>(time_delta);
    for (auto& pc: _all_classes) {
        pc->_accumulated *= normalize_factor();
    }
}

std::chrono::microseconds fair_queue_ticket::duration_at_pace(float weight_pace, float size_pace) const noexcept {
    unsigned long dur = ((_weight * weight_pace) + (_size * size_pace));
    return std::chrono::microseconds(dur);
}

bool fair_queue::grab_pending_capacity(fair_queue_ticket cap) noexcept {
    fair_group_rover pending_head = _pending->orig_tail + cap;
    if (pending_head.maybe_ahead_of(_group.head())) {
        return false;
    }

    if (cap == _pending->cap) {
        _pending.reset();
    } else {
        /*
         * This branch is called when the fair queue decides to
         * submit not the same request that entered it into the
         * pending state and this new request crawls through the
         * expected head value.
         */
        _group.grab_capacity(cap);
        _pending->orig_tail += cap;
    }

    return true;
}

bool fair_queue::grab_capacity(fair_queue_ticket cap) noexcept {
    if (_pending) {
        return grab_pending_capacity(cap);
    }

    fair_group_rover orig_tail = _group.grab_capacity(cap);
    if ((orig_tail + cap).maybe_ahead_of(_group.head())) {
        _pending.emplace(orig_tail, cap);
        return false;
    }

    return true;
}

priority_class_ptr fair_queue::register_priority_class(uint32_t shares) {
    priority_class_ptr pclass = make_lw_shared<priority_class>(shares);
    _all_classes.insert(pclass);
    return pclass;
}

void fair_queue::unregister_priority_class(priority_class_ptr pclass) {
    assert(pclass->_queue.empty());
    _all_classes.erase(pclass);
}

size_t fair_queue::waiters() const {
    return _requests_queued;
}

size_t fair_queue::requests_currently_executing() const {
    return _requests_executing;
}

fair_queue_ticket fair_queue::resources_currently_waiting() const {
    return _resources_queued;
}

fair_queue_ticket fair_queue::resources_currently_executing() const {
    return _resources_executing;
}

void fair_queue::queue(priority_class_ptr pc, fair_queue_entry& ent) {
    // We need to return a future in this function on which the caller can wait.
    // Since we don't know which queue we will use to execute the next request - if ours or
    // someone else's, we need a separate promise at this point.
    push_priority_class(pc);
    pc->_queue.push_back(ent);
    _resources_queued += ent._ticket;
    _requests_queued++;
}

void fair_queue::notify_requests_finished(fair_queue_ticket desc, unsigned nr) noexcept {
    _resources_executing -= desc;
    _requests_executing -= nr;
    _group.release_capacity(desc);
}

void fair_queue::notify_request_cancelled(fair_queue_entry& ent) noexcept {
    _resources_queued -= ent._ticket;
    ent._ticket = fair_queue_ticket();
}

void fair_queue::dispatch_requests(std::function<void(fair_queue_entry&)> cb) {
    while (_requests_queued) {
        priority_class_ptr h;

        while (true) {
            h = peek_priority_class();
            if (!h->_queue.empty()) {
                break;
            }
            pop_priority_class(h);
        }

        auto& req = h->_queue.front();
        if (!grab_capacity(req._ticket)) {
            break;
        }

        pop_priority_class(h);
        h->_queue.pop_front();

        _resources_executing += req._ticket;
        _resources_queued -= req._ticket;
        _requests_executing++;
        _requests_queued--;

        auto delta = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _base);
        auto req_cost  = req._ticket.normalize(_group.maximum_capacity()) / h->_shares;
        auto cost  = expf(1.0f/_config.tau.count() * delta.count()) * req_cost;
        float next_accumulated = h->_accumulated + cost;
        while (std::isinf(next_accumulated)) {
            normalize_stats();
            // If we have renormalized, our time base will have changed. This should happen very infrequently
            delta = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _base);
            cost  = expf(1.0f/_config.tau.count() * delta.count()) * req_cost;
            next_accumulated = h->_accumulated + cost;
        }
        h->_accumulated = next_accumulated;

        if (!h->_queue.empty()) {
            push_priority_class(h);
        }

        cb(req);
    }
}

}
