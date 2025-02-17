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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#pragma once

#include <seastar/core/circular_buffer.hh>
#include <seastar/core/future.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/modules.hh>
#ifndef SEASTAR_MODULE
#include <optional>
#include <queue>
#endif

namespace seastar {

/// Asynchronous single-producer single-consumer queue with limited capacity.
/// There can be at most one producer-side and at most one consumer-side operation active at any time.
/// Operations returning a future are considered to be active until the future resolves.
///
/// Note: queue requires the data type T to be nothrow move constructible as it's
/// returned as future<T> by \ref pop_eventually and seastar futurized data type
/// are required to be nothrow move-constructible.
SEASTAR_MODULE_EXPORT
template <typename T>
requires std::is_nothrow_move_constructible_v<T>
class queue {
    std::queue<T, circular_buffer<T>> _q;
    size_t _max;
    std::optional<promise<>> _not_empty;
    std::optional<promise<>> _not_full;
    std::exception_ptr _ex = nullptr;
private:
    void notify_not_empty() noexcept;
    void notify_not_full() noexcept;
public:
    explicit queue(size_t size);

    /// \brief Push an item.
    ///
    /// Returns false if the queue was full and the item was not pushed.
    bool push(T&& a);

    /// \brief Pop an item.
    ///
    /// Popping from an empty queue will result in undefined behavior.
    T pop() noexcept;

    /// \brief access the front element in the queue
    ///
    /// Accessing the front of an empty or aborted queue will result in undefined
    /// behaviour.
    T& front() noexcept;

    /// Consumes items from the queue, passing them to \c func, until \c func
    /// returns false or the queue it empty
    ///
    /// Returns false if func returned false.
    template <typename Func>
    bool consume(Func&& func);

    /// Returns true when the queue is empty.
    bool empty() const noexcept;

    /// Returns true when the queue is full.
    bool full() const noexcept;

    /// Returns a future<> that becomes available when pop() or consume()
    /// can be called.
    /// A consumer-side operation. Cannot be called concurrently with other consumer-side operations.
    future<> not_empty() noexcept;

    /// Returns a future<> that becomes available when push() can be called.
    /// A producer-side operation. Cannot be called concurrently with other producer-side operations.
    future<> not_full() noexcept;

    /// Pops element now or when there is some. Returns a future that becomes
    /// available when some element is available.
    /// If the queue is, or already was, abort()ed, the future resolves with
    /// the exception provided to abort().
    /// A consumer-side operation. Cannot be called concurrently with other consumer-side operations.
    future<T> pop_eventually() noexcept;

    /// Pushes the element now or when there is room. Returns a future<> which
    /// resolves when data was pushed.
    /// If the queue is, or already was, abort()ed, the future resolves with
    /// the exception provided to abort().
    /// A producer-side operation. Cannot be called concurrently with other producer-side operations.
    future<> push_eventually(T&& data) noexcept;

    /// Returns the number of items currently in the queue.
    size_t size() const noexcept {
        // std::queue::size() has no reason to throw
        return _q.size();
    }

    /// Returns the size limit imposed on the queue during its construction
    /// or by a call to set_max_size(). If the queue contains max_size()
    /// items (or more), further items cannot be pushed until some are popped.
    size_t max_size() const noexcept { return _max; }

    /// Set the maximum size to a new value. If the queue's max size is reduced,
    /// items already in the queue will not be expunged and the queue will be temporarily
    /// bigger than its max_size.
    void set_max_size(size_t max) noexcept {
        _max = max;
        if (!full()) {
            notify_not_full();
        }
    }

    /// Destroy any items in the queue, and pass the provided exception to any
    /// waiting readers or writers - or to any later read or write attempts.
    void abort(std::exception_ptr ex) noexcept {
        // std::queue::empty() and pop() doesn't throw
        // since it just calls seastar::circular_buffer::pop_front
        // that is specified as noexcept.
        while (!_q.empty()) {
            _q.pop();
        }
        _ex = ex;
        if (_not_full) {
            _not_full->set_exception(ex);
            _not_full= std::nullopt;
        }
        if (_not_empty) {
            _not_empty->set_exception(std::move(ex));
            _not_empty = std::nullopt;
        }
    }

    /// \brief Check if there is an active consumer
    ///
    /// Returns true if another fiber waits for an item to be pushed into the queue
    bool has_blocked_consumer() const noexcept {
        return bool(_not_empty);
    }
};

template <typename T>
requires std::is_nothrow_move_constructible_v<T>
inline
queue<T>::queue(size_t size)
    : _max(size) {
}

template <typename T>
requires std::is_nothrow_move_constructible_v<T>
inline
void queue<T>::notify_not_empty() noexcept {
    if (_not_empty) {
        _not_empty->set_value();
        _not_empty = std::optional<promise<>>();
    }
}

template <typename T>
requires std::is_nothrow_move_constructible_v<T>
inline
void queue<T>::notify_not_full() noexcept {
    if (_not_full) {
        _not_full->set_value();
        _not_full = std::optional<promise<>>();
    }
}

template <typename T>
requires std::is_nothrow_move_constructible_v<T>
inline
bool queue<T>::push(T&& data) {
    if (_q.size() < _max) {
        _q.push(std::move(data));
        notify_not_empty();
        return true;
    } else {
        return false;
    }
}

template <typename T>
requires std::is_nothrow_move_constructible_v<T>
inline
T& queue<T>::front() noexcept {
    // std::queue::front() has no reason to throw
    return _q.front();
}

template <typename T>
requires std::is_nothrow_move_constructible_v<T>
inline
T queue<T>::pop() noexcept {
    if (_q.size() == _max) {
        notify_not_full();
    }
    // popping the front element must not throw
    // as T is required to be nothrow_move_constructible
    // and std::queue::pop won't throw since it uses
    // seastar::circular_beffer::pop_front.
    SEASTAR_ASSERT(!_q.empty());
    T data = std::move(_q.front());
    _q.pop();
    return data;
}

template <typename T>
requires std::is_nothrow_move_constructible_v<T>
inline
future<T> queue<T>::pop_eventually() noexcept {
    // seastar allows only nothrow_move_constructible types
    // to be returned as future<T>
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "Queue element type must be no-throw move constructible");

    if (_ex) {
        return make_exception_future<T>(_ex);
    }
    if (empty()) {
        return not_empty().then([this] {
            if (_ex) {
                return make_exception_future<T>(_ex);
            } else {
                return make_ready_future<T>(pop());
            }
        });
    } else {
        return make_ready_future<T>(pop());
    }
}

template <typename T>
requires std::is_nothrow_move_constructible_v<T>
inline
future<> queue<T>::push_eventually(T&& data) noexcept {
    if (_ex) {
        return make_exception_future<>(_ex);
    }
    if (full()) {
        return not_full().then([this, data = std::move(data)] () mutable {
            _q.push(std::move(data));
            notify_not_empty();
        });
    } else {
      try {
        _q.push(std::move(data));
        notify_not_empty();
        return make_ready_future<>();
      } catch (...) {
        return current_exception_as_future();
      }
    }
}

template <typename T>
requires std::is_nothrow_move_constructible_v<T>
template <typename Func>
inline
bool queue<T>::consume(Func&& func) {
    if (_ex) {
        std::rethrow_exception(_ex);
    }
    bool running = true;
    while (!_q.empty() && running) {
        running = func(std::move(_q.front()));
        _q.pop();
    }
    if (!full()) {
        notify_not_full();
    }
    return running;
}

template <typename T>
requires std::is_nothrow_move_constructible_v<T>
inline
bool queue<T>::empty() const noexcept {
    // std::queue::empty() has no reason to throw
    return _q.empty();
}

template <typename T>
requires std::is_nothrow_move_constructible_v<T>
inline
bool queue<T>::full() const noexcept {
    // std::queue::size() has no reason to throw
    return _q.size() >= _max;
}

template <typename T>
requires std::is_nothrow_move_constructible_v<T>
inline
future<> queue<T>::not_empty() noexcept {
    if (_ex) {
        return make_exception_future<>(_ex);
    }
    if (!empty()) {
        return make_ready_future<>();
    } else {
        _not_empty = promise<>();
        return _not_empty->get_future();
    }
}

template <typename T>
requires std::is_nothrow_move_constructible_v<T>
inline
future<> queue<T>::not_full() noexcept {
    if (_ex) {
        return make_exception_future<>(_ex);
    }
    if (!full()) {
        return make_ready_future<>();
    } else {
        _not_full = promise<>();
        return _not_full->get_future();
    }
}

}

