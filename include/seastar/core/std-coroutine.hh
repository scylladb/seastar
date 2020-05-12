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
 * Copyright (C) 2019 ScyllaDB Ltd.
 */

#pragma once

#if __cplusplus > 201703L
#include <version>
#endif

#if !defined(__cpp_lib_coroutine) && !defined(SEASTAR_COROUTINES_TS)
#error Coroutines TS support disabled.
#endif

#if __has_include(<coroutine>)
#include <coroutine>
#elif __has_include(<experimental/coroutine>)
#include <experimental/coroutine>
#else

// We are not exactly allowed to defined anything in the std namespace, but this
// makes coroutines work with libstdc++. All of this is experimental anyway.

namespace std::experimental {

template<typename Promise>
class coroutine_handle {
    void* _pointer = nullptr;
public:
    coroutine_handle() = default;

    coroutine_handle &operator=(nullptr_t) noexcept {
        _pointer = nullptr;
        return *this;
    }

    explicit operator bool() const noexcept { return _pointer; }

    static coroutine_handle from_address(void* ptr) noexcept {
        coroutine_handle hndl;
        hndl._pointer =ptr;
        return hndl;
    }
    void* address() const noexcept { return _pointer; }

    static coroutine_handle from_promise(Promise& promise) noexcept {
        coroutine_handle hndl;
        hndl._pointer = __builtin_coro_promise(&promise, alignof(Promise), true);
        return hndl;
    }
    Promise& promise() const noexcept {
        return *reinterpret_cast<Promise*>(__builtin_coro_promise(_pointer, alignof(Promise), false));
    }

    void operator()() noexcept { resume(); }

    void resume() const noexcept { __builtin_coro_resume(_pointer); }
    void destroy() const noexcept { __builtin_coro_destroy(_pointer); }
    bool done() const noexcept { return __builtin_coro_done(_pointer); }
};

struct suspend_never {
    constexpr bool await_ready() const noexcept { return true; }
    template<typename T>
    constexpr void await_suspend(coroutine_handle<T>) noexcept { }
    constexpr void await_resume() noexcept { }
};

struct suspend_always {
    constexpr bool await_ready() const noexcept { return false; }
    template<typename T>
    constexpr void await_suspend(coroutine_handle<T>) noexcept { }
    constexpr void await_resume() noexcept { }
};

template<typename T, typename... Args>
class coroutine_traits { };

}

#endif
