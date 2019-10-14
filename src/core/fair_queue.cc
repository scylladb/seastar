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

#include <seastar/core/fair_queue.hh>
#include <seastar/core/future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/util/noncopyable_function.hh>
#include <queue>
#include <chrono>
#include <unordered_set>
#include <cmath>

namespace seastar {

void fair_queue::push_priority_class(priority_class_ptr pc) {
    if (!pc->_queued) {
        _handles.push(pc);
        pc->_queued = true;
    }
}

priority_class_ptr fair_queue::pop_priority_class() {
    assert(!_handles.empty());
    auto h = _handles.top();
    _handles.pop();
    assert(h->_queued);
    h->_queued = false;
    return h;
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

bool fair_queue::can_dispatch() const {
    return _requests_queued &&
           (_requests_executing < _config.capacity) &&
           (_req_count_executing < _config.max_req_count) &&
           (_bytes_count_executing < _config.max_bytes_count);
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

void fair_queue::queue(priority_class_ptr pc, fair_queue_request_descriptor desc, noncopyable_function<void()> func) {
    // We need to return a future in this function on which the caller can wait.
    // Since we don't know which queue we will use to execute the next request - if ours or
    // someone else's, we need a separate promise at this point.
    push_priority_class(pc);
    pc->_queue.push_back(priority_class::request{std::move(func), std::move(desc)});
    _requests_queued++;
}

void fair_queue::notify_requests_finished(fair_queue_request_descriptor& desc) {
    _requests_executing--;
    _req_count_executing -= desc.weight;
    _bytes_count_executing -= desc.size;
}


void fair_queue::dispatch_requests() {
    while (can_dispatch()) {
        priority_class_ptr h;
        do {
            h = pop_priority_class();
        } while (h->_queue.empty());

        auto req = std::move(h->_queue.front());
        h->_queue.pop_front();
        _requests_executing++;
        _req_count_executing += req.desc.weight;
        _bytes_count_executing += req.desc.size;
        _requests_queued--;

        auto delta = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - _base);
        auto req_cost  = (float(req.desc.weight) / _config.max_req_count + float(req.desc.size) / _config.max_bytes_count) / h->_shares;
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
        req.func();
    }
}

void fair_queue::update_shares(priority_class_ptr pc, uint32_t new_shares) {
    pc->update_shares(new_shares);
}

}
