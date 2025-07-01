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

#ifdef SEASTAR_MODULE
module;
#endif

#include <chrono>
#include <functional>
#include <utility>
#include <boost/container/small_vector.hpp>
#include <boost/intrusive/parent_from_member.hpp>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/fair_queue.hh>
#include <seastar/core/future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/core/metrics.hh>
#endif
#include <seastar/util/assert.hh>

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

bool fair_queue_ticket::is_non_zero() const noexcept {
    return (_weight > 0) && (_size > 0);
}

bool fair_queue_ticket::operator==(const fair_queue_ticket& o) const noexcept {
    return _weight == o._weight && _size == o._size;
}

std::ostream& operator<<(std::ostream& os, fair_queue_ticket t) {
    return os << t._weight << ":" << t._size;
}

fair_queue_ticket wrapping_difference(const fair_queue_ticket& a, const fair_queue_ticket& b) noexcept {
    return fair_queue_ticket(std::max<int32_t>(a._weight - b._weight, 0),
            std::max<int32_t>(a._size - b._size, 0));
}

// Priority class, to be used with a given fair_queue
class fair_queue::priority_class_data {
    friend class fair_queue;
    uint32_t _shares = 0;
    capacity_t _accumulated = 0;
    capacity_t _pure_accumulated = 0;
    fair_queue_entry::container_list_t _queue;
    bool _queued = false;
    bool _plugged = true;
    uint32_t _activations = 0;

public:
    explicit priority_class_data(uint32_t shares) noexcept : _shares(std::max(shares, 1u)) {}
    priority_class_data(const priority_class_data&) = delete;
    priority_class_data(priority_class_data&&) = delete;

    void update_shares(uint32_t shares) noexcept {
        _shares = (std::max(shares, 1u));
    }
};

bool fair_queue::class_compare::operator() (const priority_class_ptr& lhs, const priority_class_ptr & rhs) const noexcept {
    return lhs->_accumulated > rhs->_accumulated;
}

fair_queue::fair_queue(config cfg)
    : _config(std::move(cfg))
{
}

fair_queue::~fair_queue() {
    for (const auto& fq : _priority_classes) {
        SEASTAR_ASSERT(!fq);
    }
}

void fair_queue::push_priority_class(priority_class_data& pc) noexcept {
    SEASTAR_ASSERT(pc._plugged && !pc._queued);
    _handles.assert_enough_capacity();
    _handles.push(&pc);
    pc._queued = true;
}

void fair_queue::push_priority_class_from_idle(priority_class_data& pc) noexcept {
    if (!pc._queued) {
        // Don't let the newcomer monopolize the disk for more than tau
        // duration. For this estimate how many capacity units can be
        // accumulated with the current class shares per rate resulution
        // and scale it up to tau.
        // On start this deviation can go to negative values, so not to
        // introduce extra if's for that short corner case, use signed
        // arithmetics and make sure the _accumulated value doesn't grow
        // over signed maximum (see overflow check below)
        pc._accumulated = std::max<signed_capacity_t>(_last_accumulated - _config.forgiving_factor / pc._shares, pc._accumulated);
        _handles.assert_enough_capacity();
        _handles.push(&pc);
        pc._queued = true;
        pc._activations++;
    }
}

// ATTN: This can only be called on pc that is from _handles.top()
void fair_queue::pop_priority_class(priority_class_data& pc) noexcept {
    SEASTAR_ASSERT(pc._queued);
    pc._queued = false;
    _handles.pop();
}

void fair_queue::plug_priority_class(priority_class_data& pc) noexcept {
    SEASTAR_ASSERT(!pc._plugged);
    pc._plugged = true;
    if (!pc._queue.empty()) {
        push_priority_class_from_idle(pc);
    }
}

void fair_queue::plug_class(class_id cid) noexcept {
    plug_priority_class(*_priority_classes[cid]);
}

void fair_queue::unplug_priority_class(priority_class_data& pc) noexcept {
    SEASTAR_ASSERT(pc._plugged);
    pc._plugged = false;
}

void fair_queue::unplug_class(class_id cid) noexcept {
    unplug_priority_class(*_priority_classes[cid]);
}

fair_queue::capacity_t fair_queue::accumulated(class_id cid) const noexcept {
    return _priority_classes[cid]->_accumulated;
}

fair_queue::capacity_t fair_queue::pure_accumulated(class_id cid) const noexcept {
    return _priority_classes[cid]->_pure_accumulated;
}

unsigned fair_queue::activations(class_id cid) const noexcept {
    return _priority_classes[cid]->_activations;
}

void fair_queue::register_priority_class(class_id id, uint32_t shares) {
    if (id >= _priority_classes.size()) {
        _priority_classes.resize(id + 1);
    } else {
        SEASTAR_ASSERT(!_priority_classes[id]);
    }

    _handles.reserve(_nr_classes + 1);
    _priority_classes[id] = std::make_unique<priority_class_data>(shares);
    _nr_classes++;
}

void fair_queue::unregister_priority_class(class_id id) {
    auto& pclass = _priority_classes[id];
    SEASTAR_ASSERT(pclass);
    pclass.reset();
    _nr_classes--;
}

void fair_queue::update_shares_for_class(class_id id, uint32_t shares) {
    SEASTAR_ASSERT(id < _priority_classes.size());
    auto& pc = _priority_classes[id];
    SEASTAR_ASSERT(pc);
    pc->update_shares(shares);
}

fair_queue_ticket fair_queue::resources_currently_waiting() const {
    return _resources_queued;
}

fair_queue_ticket fair_queue::resources_currently_executing() const {
    return _resources_executing;
}

void fair_queue::queue(class_id id, fair_queue_entry& ent) noexcept {
    priority_class_data& pc = *_priority_classes[id];
    // We need to return a future in this function on which the caller can wait.
    // Since we don't know which queue we will use to execute the next request - if ours or
    // someone else's, we need a separate promise at this point.
    if (pc._plugged) {
        push_priority_class_from_idle(pc);
    }
    pc._queue.push_back(ent);
    _queued_capacity += ent.capacity();
}

void fair_queue::notify_request_finished(fair_queue_entry::capacity_t cap) noexcept {
}

void fair_queue::notify_request_cancelled(fair_queue_entry& ent) noexcept {
    _queued_capacity -= ent._capacity;
    ent._capacity = 0;
}

fair_queue_entry* fair_queue::top() {
    while (!_handles.empty()) {
        priority_class_data& h = *_handles.top();
        if (h._queue.empty() || !h._plugged) {
            pop_priority_class(h);
            continue;
        }

        return &h._queue.front();
    }

    return nullptr;
}

void fair_queue::pop_front() {
    auto& h = *_handles.top();

    _last_accumulated = std::max(h._accumulated, _last_accumulated);
    pop_priority_class(h);

    auto& req = h._queue.front();
    h._queue.pop_front();

    // Usually the cost of request is tens to hundreeds of thousands. However, for
    // unrestricted queue it can be as low as 2k. With large enough shares this
    // has chances to be translated into zero cost which, in turn, will make the
    // class show no progress and monopolize the queue.
    auto req_cap = req._capacity;
    auto req_cost  = std::max(req_cap / h._shares, (capacity_t)1);
    // signed overflow check to make push_priority_class_from_idle math work
    if (h._accumulated >= std::numeric_limits<signed_capacity_t>::max() - req_cost) {
        for (auto& pc : _priority_classes) {
            if (pc) {
                if (pc->_queued) {
                    pc->_accumulated -= h._accumulated;
                } else { // this includes h
                    pc->_accumulated = 0;
                }
            }
        }
        _last_accumulated = 0;
    }
    h._accumulated += req_cost;
    h._pure_accumulated += req_cap;
    _queued_capacity -= req_cap;

    if (!h._queue.empty()) {
        push_priority_class(h);
    }
}

}
