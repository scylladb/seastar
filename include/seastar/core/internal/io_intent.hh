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

#include <seastar/util/assert.hh>
#include <seastar/util/modules.hh>
#ifndef SEASTAR_MODULE
#include <utility>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/slist.hpp>
#endif

namespace bi = boost::intrusive;

namespace seastar {

SEASTAR_MODULE_EXPORT
class io_intent;

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
        ~link() { SEASTAR_ASSERT(_ref == nullptr); }

        void enqueue(cancellable_queue& cq) noexcept {
            cq.push_back(*this);
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

/*
 * A "safe" reference on a intent. Safe here means that the original
 * intent can be destroyed at any time and this reference will be
 * updated not to point at it any longer.
 * The retrieve() method brings the original intent back or throws
 * and exception if the intent was cancelled.
 */
class intent_reference : public bi::list_base_hook<bi::link_mode<bi::auto_unlink>> {
    friend class seastar::io_intent;
    using container_type = bi::list<intent_reference, bi::constant_time_size<false>>;
    static constexpr uintptr_t _cancelled_intent = 1;
    io_intent* _intent;

    void on_cancel() noexcept { _intent = reinterpret_cast<io_intent*>(_cancelled_intent); }
    bool is_cancelled() const noexcept { return _intent == reinterpret_cast<io_intent*>(_cancelled_intent); }

public:
    intent_reference(io_intent* intent) noexcept;

    intent_reference(intent_reference&& o) noexcept : _intent(std::exchange(o._intent, nullptr)) {
        container_type::node_algorithms::swap_nodes(o.this_ptr(), this_ptr());
    }

    intent_reference& operator=(intent_reference&& o) noexcept {
        if (this != &o) {
            _intent = std::exchange(o._intent, nullptr);
            unlink();
            container_type::node_algorithms::swap_nodes(o.this_ptr(), this_ptr());
        }
        return *this;
    }

    io_intent* retrieve() const;
};

} // namespace internal

} // namespace seastar
