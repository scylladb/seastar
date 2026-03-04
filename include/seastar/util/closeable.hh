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
 * Copyright (C) 2021 Cloudius Systems, Ltd.
 */

#pragma once

#include <concepts>
#include <functional>
#include <seastar/core/future.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/defer.hh>

/// \file

/// \example closeable_test.cc

namespace seastar {

template <typename Object>
concept closeable = requires (Object o) {
    { o.close() } SEASTAR_DEFERRED_ACTION_NOEXCEPT -> std::same_as<future<>>;
};

/// Template helper to auto-close \c obj when destroyed.
///
/// \tparam Object a class exposing a \c close() method that returns a \c future<>
///         that is called when the controller is destroyed.
///
/// Must be used in a seastar thread as the destructor
/// needs to wait on the \c obj close() future.
template <typename Object>
requires closeable<Object>
class [[nodiscard]] deferred_close {
    std::reference_wrapper<Object> _obj;
    bool _closed = false;

    void do_close() noexcept {
        if (!_closed) {
            _closed = true;
            _obj.get().close().get();
        }
    }
public:
    /// Construct an object that will auto-close \c obj when destroyed.
    /// \tparam obj the object to auto-close.
    deferred_close(Object& obj) noexcept : _obj(obj) {}
    /// Moves the \c deferred_close into a new one, and
    /// the old one is canceled.
    deferred_close(deferred_close&& x) noexcept : _obj(x._obj), _closed(std::exchange(x._closed, true)) {}
    deferred_close(const deferred_close&) = delete;
    /// Move-assign another \ref deferred_close.
    /// The current \ref deferred_close is closed before being assigned.
    /// And the other one's state is transferred to the current one.
    deferred_close& operator=(deferred_close&& x) noexcept {
        do_close();
        _obj = x._obj;
        _closed = std::exchange(x._closed, true);
        return *this;
    }
    /// Destruct the deferred_close object and auto-close \c obj.
    ~deferred_close() {
        do_close();
    }
    /// Close \c obj once now.
    void close_now() noexcept {
        SEASTAR_ASSERT(!_closed);
        do_close();
    }

    /// Prevents close() from being called when this object is destroyed.
    /// Cannot call close_now() any more after this.
    void cancel() noexcept {
        _closed = true;
    }
};

template <closeable Closeable, std::invocable<Closeable&> Func>
requires std::is_nothrow_move_constructible_v<Closeable> && std::is_nothrow_move_constructible_v<Func>
inline futurize_t<std::invoke_result_t<Func, Closeable&>>
with_closeable(Closeable&& obj, Func func) noexcept {
    return do_with(std::move(obj), [func = std::move(func)] (Closeable& obj) mutable {
        return futurize_invoke(func, obj).finally([&obj] {
            return obj.close();
        });
    });
}

template <typename Object>
concept stoppable = requires (Object o) {
    { o.stop() } SEASTAR_DEFERRED_ACTION_NOEXCEPT -> std::same_as<future<>>;
};

/// Template helper to auto-stop \c obj when destroyed.
///
/// \tparam Object a class exposing a \c stop() method that returns a \c future<>
///         that is called when the controller is destroyed.
///
/// Must be used in a seastar thread as the destructor
/// needs to wait on the \c obj stop() future.
template <typename Object>
requires stoppable<Object>
class [[nodiscard]] deferred_stop {
    std::reference_wrapper<Object> _obj;
    bool _stopped = false;

    void do_stop() noexcept {
        if (!_stopped) {
            _stopped = true;
            _obj.get().stop().get();
        }
    }
public:
    /// Construct an object that will auto-stop \c obj when destroyed.
    /// \tparam obj the object to auto-stop.
    deferred_stop(Object& obj) noexcept : _obj(obj) {}
    /// Moves the \c deferred_stop into a new one, and
    /// the old one is canceled.
    deferred_stop(deferred_stop&& x) noexcept : _obj(x._obj), _stopped(std::exchange(x._stopped, true)) {}
    deferred_stop(const deferred_stop&) = delete;
    /// Move-assign another \ref deferred_stop.
    /// The current \ref deferred_stop is stopped before being assigned.
    /// And the other one's state is transferred to the current one.
    deferred_stop& operator=(deferred_stop&& x) noexcept {
        do_stop();
        _obj = x._obj;
        _stopped = std::exchange(x._stopped, true);
        return *this;
    }
    /// Destruct the deferred_stop object and auto-stop \c obj.
    ~deferred_stop() {
        do_stop();
    }
    /// Stop \c obj once now.
    void stop_now() noexcept {
        SEASTAR_ASSERT(!_stopped);
        do_stop();
    }

    /// Prevents stop() from being called when this object is destroyed.
    /// Cannot call stop_now() any more after this.
    void cancel() noexcept {
        _stopped = true;
    }
};

template <stoppable Stoppable, std::invocable<Stoppable&> Func>
requires std::is_nothrow_move_constructible_v<Stoppable> && std::is_nothrow_move_constructible_v<Func>
inline futurize_t<std::invoke_result_t<Func, Stoppable&>>
with_stoppable(Stoppable&& obj, Func func) noexcept {
    return do_with(std::move(obj), [func = std::move(func)] (Stoppable& obj) mutable {
        return futurize_invoke(func, obj).finally([&obj] {
            return obj.stop();
        });
    });
}

} // namespace seastar
