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

#ifndef SEASTAR_MODULE
#include <seastar/core/future.hh>
#include <seastar/core/chunked_fifo.hh>
#include <exception>
#include <memory>
#include <seastar/core/timer.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/timed_out_error.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/modules.hh>
#endif

namespace seastar {

template<typename T>
struct dummy_expiry {
    void operator()(T&) noexcept {};
};

template<typename... T>
struct promise_expiry {
    void operator()(promise<T...>& pr) noexcept {
        pr.set_exception(std::make_exception_ptr(timed_out_error()));
    };
};

/// Container for elements with support for expiration of entries.
///
/// OnExpiry is a functor which will be called with a reference to T right before it expires.
/// T is removed and destroyed from the container immediately after OnExpiry returns.
/// OnExpiry callback must not modify the container, it can only modify its argument.
///
/// The container can only be moved before any elements are pushed.
///
SEASTAR_MODULE_EXPORT
template <typename T, typename OnExpiry = dummy_expiry<T>, typename Clock = lowres_clock>
class expiring_fifo {
public:
    using clock = Clock;
    using time_point = typename Clock::time_point;
private:
    struct entry {
        std::optional<T> payload; // disengaged means that it's expired
        timer<Clock> tr;
        entry(T&& payload_) : payload(std::move(payload_)) {}
        entry(const T& payload_) : payload(payload_) {}
        entry(T payload_, expiring_fifo& ef, time_point timeout)
                : payload(std::move(payload_))
                , tr([this, &ef] {
                    ef._on_expiry(*payload);
                    payload = std::nullopt;
                    --ef._size;
                    ef.drop_expired_front();
                })
        {
            tr.arm(timeout);
        }
        entry(entry&& x) = delete;
        entry(const entry& x) = delete;
    };

    // If engaged, represents the first element.
    // This is to avoid large allocations done by chunked_fifo for single-element cases.
    // expiring_fifo is used to implement wait lists in synchronization primitives
    // and in some uses it's common to have at most one waiter.
    std::unique_ptr<entry> _front;

    // There is an invariant that the front element is never expired.
    chunked_fifo<entry> _list;
    OnExpiry _on_expiry;
    size_t _size = 0;

    // Ensures that front() is not expired by dropping expired elements from the front.
    void drop_expired_front() noexcept {
        while (!_list.empty() && !_list.front().payload) {
            _list.pop_front();
        }
        if (_front && !_front->payload) {
            _front.reset();
        }
    }
public:
    expiring_fifo() noexcept = default;
    expiring_fifo(OnExpiry on_expiry) noexcept(std::is_nothrow_move_constructible_v<OnExpiry>) : _on_expiry(std::move(on_expiry)) {}

    expiring_fifo(expiring_fifo&& o) noexcept
            : expiring_fifo(std::move(o._on_expiry)) {
        // entry objects hold a reference to this so non-empty containers cannot be moved.
        SEASTAR_ASSERT(o._size == 0);
    }

    expiring_fifo& operator=(expiring_fifo&& o) noexcept {
        if (this != &o) {
            this->~expiring_fifo();
            new (this) expiring_fifo(std::move(o));
        }
        return *this;
    }

    /// Checks if container contains any elements
    ///
    /// \note Inside OnExpiry callback, the expired element is still contained.
    ///
    /// \return true if and only if there are any elements contained.
    bool empty() const noexcept {
        return _size == 0;
    }

    /// Equivalent to !empty()
    explicit operator bool() const noexcept {
        return !empty();
    }

    /// Returns a reference to the element in the front.
    /// Valid only when !empty().
    T& front() noexcept {
        if (_front) {
            return *_front->payload;
        }
        return *_list.front().payload;
    }

    /// Returns a reference to the element in the front.
    /// Valid only when !empty().
    const T& front() const noexcept {
        if (_front) {
            return *_front->payload;
        }
        return *_list.front().payload;
    }

    /// Returns the number of elements contained.
    ///
    /// \note Expired elements are not contained. Expiring element is still contained when OnExpiry is called.
    size_t size() const noexcept {
        return _size;
    }

    /// Reserves storage in the container for at least 'size' elements.
    /// Note that expired elements may also take space when they are not in the front of the queue.
    ///
    /// Doesn't give any guarantees about exception safety of subsequent push_back().
    void reserve(size_t size) {
        return _list.reserve(size);
    }

    /// Adds element to the back of the queue.
    /// The element will never expire.
    void push_back(const T& payload) {
        if (_size == 0) {
            _front = std::make_unique<entry>(payload);
        } else {
            _list.emplace_back(payload);
        }
        ++_size;
    }

    /// Adds element to the back of the queue.
    /// The element will never expire.
    void push_back(T&& payload) {
        if (_size == 0) {
            _front = std::make_unique<entry>(std::move(payload));
        } else {
            _list.emplace_back(std::move(payload));
        }
        ++_size;
    }

    /// Adds element to the back of the queue.
    /// The element will expire when timeout is reached, unless it is time_point::max(), in which
    /// case it never expires.
    void push_back(T&& payload, time_point timeout) {
        if (timeout == time_point::max()) {
            push_back(std::move(payload));
            return;
        }
        if (_size == 0) {
            _front = std::make_unique<entry>(std::move(payload), *this, timeout);
        } else {
            _list.emplace_back(std::move(payload), *this, timeout);
        }
        ++_size;
    }

    /// Removes the element at the front.
    /// Can be called only if !empty().
    void pop_front() noexcept {
        if (_front) {
            _front.reset();
        } else {
            _list.pop_front();
        }
        --_size;
        drop_expired_front();
    }
};

}
