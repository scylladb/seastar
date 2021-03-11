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

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/slist.hpp>
#include <boost/container/small_vector.hpp>
#include <seastar/core/internal/io_intent.hh>
#include <seastar/core/io_priority_class.hh>

namespace bi = boost::intrusive;

namespace seastar {

/// \example file_demo.cc
/// A handle confirming the caller's intent to do the IO
///
/// When a pointer to an intent is passed to the \ref io_queue
/// "io_queue"::queue_request() method, the issued request is pinned
/// to the intent and is only processed as long as the intent object
/// is alive and the **cancel()** method is not called.
///
/// If no intent is provided, then the request is processed till its
/// completion be it success or error
class io_intent {
    struct intents_for_queue {
        dev_t dev;
        io_priority_class_id qid;
        internal::cancellable_queue cq;

        intents_for_queue(dev_t dev_, io_priority_class_id qid_) noexcept
            : dev(dev_), qid(qid_), cq() {}

        intents_for_queue(intents_for_queue&&) noexcept = default;
        intents_for_queue& operator=(intents_for_queue&&) noexcept = default;
    };

    struct references {
        internal::intent_reference::container_type list;

        references(references&& o) noexcept : list(std::move(o.list)) {}
        references() noexcept : list() {}
        ~references() { clear(); }

        void clear() {
            list.clear_and_dispose([] (internal::intent_reference* r) { r->on_cancel(); });
        }

        void bind(internal::intent_reference& iref) noexcept {
            list.push_back(iref);
        }
    };

    boost::container::small_vector<intents_for_queue, 1> _intents;
    references _refs;
    friend internal::intent_reference::intent_reference(io_intent*) noexcept;

public:
    io_intent() = default;
    ~io_intent() = default;

    io_intent(const io_intent&) = delete;
    io_intent& operator=(const io_intent&) = delete;
    io_intent& operator=(io_intent&&) = delete;
    io_intent(io_intent&& o) noexcept : _intents(std::move(o._intents)), _refs(std::move(o._refs)) {
        for (auto&& r : _refs.list) {
            r._intent = this;
        }
    }

    /// Explicitly cancels all the requests attached to this intent
    /// so far. The respective futures are resolved into the \ref
    /// cancelled_error "cancelled_error"
    void cancel() noexcept {
        _refs.clear();
        _intents.clear();
    }

    /// @private
    internal::cancellable_queue& find_or_create_cancellable_queue(dev_t dev, io_priority_class_id qid) {
        for (auto&& i : _intents) {
            if (i.dev == dev && i.qid == qid) {
                return i.cq;
            }
        }

        _intents.emplace_back(dev, qid);
        return _intents.back().cq;
    }
};

} // namespace seastar
