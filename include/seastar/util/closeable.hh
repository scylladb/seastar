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

#include <seastar/core/future.hh>
#include <seastar/util/concepts.hh>

/// \file

/// \example closeable_test.cc

namespace seastar {

SEASTAR_CONCEPT(
template <typename Object>
concept closeable = requires (Object o) {
    { o.close() } -> std::same_as<future<>>;
};
)

/// Template helper to auto-close \c obj when destroyed.
///
/// \tparam Object a class exposing a \c close() method that returns a \c future<>
///         that is called when the controller is destroyed.
///
/// Must be used in a seastar thread as the destructor
/// needs to wait on the \c obj close() future.
template <typename Object>
SEASTAR_CONCEPT( requires closeable<Object> )
class deferred_close {
    Object& _obj;
    bool _closed = false;

    void do_close() noexcept {
        if (!_closed) {
            _closed = true;
            _obj.close().get();
        }
    }
public:
    /// Construct an object that will auto-close \c obj when destroyed.
    /// \tparam obj the object to auto-close.
    deferred_close(Object& obj) noexcept : _obj(obj) {}
    /// Destruct the deferred_close object and auto-close \c obj.
    ~deferred_close() {
        do_close();
    }
    /// Close \c obj once now.
    void close_now() noexcept {
        assert(!_closed);
        do_close();
    }
};

template <typename Closeable, typename Func>
SEASTAR_CONCEPT(
requires closeable<Closeable> && std::invocable<Func, Closeable&> &&
        std::is_nothrow_move_constructible_v<Closeable> && std::is_nothrow_move_constructible_v<Func>
)
inline futurize_t<std::invoke_result_t<Func, Closeable&>>
with_closeable(Closeable&& obj, Func func) noexcept {
    return do_with(std::move(obj), [func = std::move(func)] (Closeable& obj) mutable {
        return futurize_invoke(func, obj).finally([&obj] {
            return obj.close();
        });
    });
}

SEASTAR_CONCEPT(
template <typename Object>
concept stoppable = requires (Object o) {
    { o.stop() } -> std::same_as<future<>>;
};
)

/// Template helper to auto-stop \c obj when destroyed.
///
/// \tparam Object a class exposing a \c stop() method that returns a \c future<>
///         that is called when the controller is destroyed.
///
/// Must be used in a seastar thread as the destructor
/// needs to wait on the \c obj stop() future.
template <typename Object>
SEASTAR_CONCEPT( requires stoppable<Object> )
class deferred_stop {
    Object& _obj;
    bool _stopped = false;

    void do_stop() noexcept {
        if (!_stopped) {
            _stopped = true;
            _obj.stop().get();
        }
    }
public:
    /// Construct an object that will auto-stop \c obj when destroyed.
    /// \tparam obj the object to auto-stop.
    deferred_stop(Object& obj) noexcept : _obj(obj) {}
    /// Destruct the deferred_stop object and auto-stop \c obj.
    ~deferred_stop() {
        do_stop();
    }
    /// Stop \c obj once now.
    void stop_now() noexcept {
        assert(!_stopped);
        do_stop();
    }
};

template <typename Stoppable, typename Func>
SEASTAR_CONCEPT(
requires stoppable<Stoppable> && std::invocable<Func, Stoppable&> &&
        std::is_nothrow_move_constructible_v<Stoppable> && std::is_nothrow_move_constructible_v<Func>
)
inline futurize_t<std::invoke_result_t<Func, Stoppable&>>
with_stoppable(Stoppable&& obj, Func func) noexcept {
    return do_with(std::move(obj), [func = std::move(func)] (Stoppable& obj) mutable {
        return futurize_invoke(func, obj).finally([&obj] {
            return obj.stop();
        });
    });
}

} // namespace seastar
