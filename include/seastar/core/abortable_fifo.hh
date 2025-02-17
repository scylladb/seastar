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
 * Copyright (C) 2022 ScyllaDB
 */

#pragma once

#include <seastar/core/abort_source.hh>
#include <seastar/core/future.hh>
#include <seastar/core/chunked_fifo.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/modules.hh>

#ifndef SEASTAR_MODULE
#include <memory>
#include <optional>
#include <type_traits>
#endif

namespace seastar {

namespace internal {

// Test if aborter has call operator accepting optional exception_ptr
template <typename Aborter, typename T>
concept aborter_ex = std::is_nothrow_invocable_r_v<void, Aborter, T&, const std::optional<std::exception_ptr>&>;

template <typename Aborter, typename T>
concept aborter = std::is_nothrow_invocable_r_v<void, Aborter, T&> || aborter_ex<Aborter, T>;

// This class satisfies 'aborter' concept and is used by default
template<typename... T>
struct noop_aborter {
    void operator()(T...) noexcept {};
};


/// Container for elements with support for cancellation of entries.
///
/// OnAbort is a functor which will be called with a reference to T right before it expires.
/// T is removed and destroyed from the container immediately after OnAbort returns.
/// OnAbort callback must not modify the container, it can only modify its argument.
///
/// The container can only be moved before any elements are pushed.
///
template <typename T, typename OnAbort = noop_aborter<T>>
requires aborter<OnAbort, T>
class abortable_fifo {
private:
    struct entry {
        std::optional<T> payload; // disengaged means that it's expired
        optimized_optional<abort_source::subscription> sub;
        entry(T&& payload_) : payload(std::move(payload_)) {}
        entry(const T& payload_) : payload(payload_) {}
        entry(T payload_, abortable_fifo& ef, abort_source& as)
                : payload(std::move(payload_))
                , sub(as.subscribe([this, &ef] (const std::optional<std::exception_ptr>& ex_opt) noexcept {
                    if constexpr (aborter_ex<OnAbort, T>) {
                        ef._on_abort(*payload, ex_opt);
                    } else {
                        ef._on_abort(*payload);
                    }
                    payload = std::nullopt;
                    --ef._size;
                    ef.drop_expired_front();
                })) {}
        entry() = default;
        entry(entry&& x) = delete;
        entry(const entry& x) = delete;
    };

    // If engaged, represents the first element.
    // This is to avoid large allocations done by chunked_fifo for single-element cases.
    // abortable_fifo is used to implement wait lists in synchronization primitives
    // and in some uses it's common to have at most one waiter.
    std::unique_ptr<entry> _front;

    // There is an invariant that the front element is never expired.
    chunked_fifo<entry> _list;
    OnAbort _on_abort;
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
    abortable_fifo() noexcept = default;
    abortable_fifo(OnAbort on_abort) noexcept(std::is_nothrow_move_constructible_v<OnAbort>) : _on_abort(std::move(on_abort)) {}

    abortable_fifo(abortable_fifo&& o) noexcept
            : abortable_fifo(std::move(o._on_abort)) {
        // entry objects hold a reference to this so non-empty containers cannot be moved.
        SEASTAR_ASSERT(o._size == 0);
    }

    abortable_fifo& operator=(abortable_fifo&& o) noexcept {
        if (this != &o) {
            this->~abortable_fifo();
            new (this) abortable_fifo(std::move(o));
        }
        return *this;
    }

    /// Checks if container contains any elements
    ///
    /// \note Inside OnAbort callback, the expired element is still contained.
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
    /// \note Expired elements are not contained. Expiring element is still contained when OnAbort is called.
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
    /// The element will expire when abort source is triggered.
    void push_back(T&& payload, abort_source& as) {
        if (as.abort_requested()) {
            if constexpr (aborter_ex<OnAbort, T>) {
                _on_abort(payload, std::nullopt);
            } else {
                _on_abort(payload);
            }
            return;
        }
        if (_size == 0) {
            _front = std::make_unique<entry>(std::move(payload), *this, as);
        } else {
            _list.emplace_back(std::move(payload), *this, as);
        }
        ++_size;
    }

    /// Create element in place at the back of the queue.
    template<typename... U>
    T& emplace_back(U&&... args) {
        if (_size == 0) {
            _front = std::make_unique<entry>();
            _front->payload.emplace(std::forward<U>(args)...);
            _size = 1;
            return *_front->payload;
        } else {
            _list.emplace_back();
            _list.back().payload.emplace(std::forward<U>(args)...);
            ++_size;
            return *_list.back().payload;
        }
    }

    /// Make an element at the back of the queue abortable
    /// The element will expire when abort source is triggered.
    /// Valid only when !empty().
    /// Cannot be called on an element that si already associated
    /// with an abort source.
    void make_back_abortable(abort_source& as) {
        entry* e = _front.get();
        if (!_list.empty()) {
            e = &_list.back();
        }
        SEASTAR_ASSERT(!e->sub);
        auto aborter = [this, e] (const std::optional<std::exception_ptr>& ex_opt) noexcept {
            if constexpr (aborter_ex<OnAbort, T>) {
                _on_abort(*e->payload, ex_opt);
            } else {
                _on_abort(*e->payload);
            }
            e->payload = std::nullopt;
            --_size;
            drop_expired_front();
        };
        if (as.abort_requested()) {
            aborter(as.abort_requested_exception_ptr());
            return;
        }
        e->sub = as.subscribe(std::move(aborter));
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
}

