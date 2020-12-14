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
 * Copyright 2021 ScyllaDB
 */

#pragma once

#include <boost/intrusive/slist.hpp>

namespace bi = boost::intrusive;

namespace seastar {

namespace internal {

/*
 * The tracker of cancellable sub-queue of requests.
 *
 * This queue is stuffed with requests that sit in the
 * same IO queue for dispatching (there can be other requests
 * as well) and ties them together for cancellation.
 * This IO queue is the fair_queue's priority_class's one.
 * Beware, if requests from different IO queues happen
 * in the same cancellable queue the whole thing blows up.
 */
class cancellable_queue {
public:
    class link {
        friend class cancellable_queue;

        union {
            cancellable_queue* _ref;
            bi::slist_member_hook<> _hook;
        };

    public:
        link() noexcept : _ref(nullptr) {}
        ~link() { assert(_ref == nullptr); }

        void enqueue(cancellable_queue* cq) noexcept {
            if (cq != nullptr) {
                cq->push_back(*this);
            }
        }

        void maybe_dequeue() noexcept {
            if (_ref != nullptr) {
                _ref->pop_front();
            }
        }
    };

private:
    static_assert(sizeof(link) == sizeof(void*), "cancellable_queue::link size is too big");
    using list_of_links_t = bi::slist<link,
        bi::constant_time_size<false>,
        bi::cache_last<true>,
        bi::member_hook<link, bi::slist_member_hook<>, &link::_hook>>;

    link* _first;
    list_of_links_t _rest;

    void push_back(link& il) noexcept;
    void pop_front() noexcept;

public:
    cancellable_queue() noexcept : _first(nullptr) {}
    cancellable_queue(const cancellable_queue&) = delete;
    cancellable_queue(cancellable_queue&& o) noexcept;
    cancellable_queue& operator=(cancellable_queue&& o) noexcept;
    ~cancellable_queue();
};

} // namespace internal

} // namespace seastar
