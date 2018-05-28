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
 * Copyright (C) 2017 ScyllaDB Ltd.
 */

#pragma once

#include <utility>
#include <type_traits>
#include <functional>

namespace seastar {

template <typename Signature>
class noncopyable_function;

/// A clone of \c std::function, but only invokes the move constructor
/// of the contained function.
template <typename Ret, typename... Args>
class noncopyable_function<Ret (Args...)> {
    static constexpr size_t nr_direct = 32;
    union [[gnu::may_alias]] storage {
        char direct[nr_direct];
        void* indirect;
    };
    using call_type = Ret (*)(const noncopyable_function* func, Args...);
    using move_type = void (*)(noncopyable_function* from, noncopyable_function* to);
    using destroy_type = void (*)(noncopyable_function* func);
    struct vtable {
        const call_type call;
        const move_type move;
        const destroy_type destroy;
    };
private:
    const vtable* _vtable;
    storage _storage;
private:
    static Ret empty_call(const noncopyable_function* func, Args... args) {
        throw std::bad_function_call();
    }
    static void empty_move(noncopyable_function* from, noncopyable_function* to) {}
    static void empty_destroy(noncopyable_function* func) {}
    static constexpr vtable _s_empty_vtable = {empty_call, empty_move, empty_destroy};

    static void indirect_move(noncopyable_function* from, noncopyable_function* to) {
        using void_ptr = void*;
        new (&to->_storage.indirect) void_ptr(from->_storage.indirect);
    }

    template <typename Func>
    struct direct_vtable_for {
        static Func* access(noncopyable_function* func) { return reinterpret_cast<Func*>(func->_storage.direct); }
        static const Func* access(const noncopyable_function* func) { return reinterpret_cast<const Func*>(func->_storage.direct); }
        static Ret call(const noncopyable_function* func, Args... args) {
            return (*access(const_cast<noncopyable_function*>(func)))(std::forward<Args>(args)...);
        }
        static void move(noncopyable_function* from, noncopyable_function* to) {
            new (access(to)) Func(std::move(*access(from)));
            destroy(from);
        }
        static void destroy(noncopyable_function* func) {
            access(func)->~Func();
        }
        static void initialize(Func&& from, noncopyable_function* to) {
            new (access(to)) Func(std::move(from));
        }
        static constexpr vtable make_vtable() { return { call, move, destroy }; }
        static const vtable s_vtable;
    };
    template <typename Func>
    struct indirect_vtable_for {
        static Func* access(noncopyable_function* func) { return reinterpret_cast<Func*>(func->_storage.indirect); }
        static const Func* access(const noncopyable_function* func) { return reinterpret_cast<const Func*>(func->_storage.indirect); }
        static Ret call(const noncopyable_function* func, Args... args) {
            return (*access(const_cast<noncopyable_function*>(func)))(std::forward<Args>(args)...);
        }
        static void destroy(noncopyable_function* func) {
            delete access(func);
        }
        static void initialize(Func&& from, noncopyable_function* to) {
            to->_storage.indirect = new Func(std::move(from));
        }
        static constexpr vtable make_vtable() { return { call, indirect_move, destroy }; }
        static const vtable s_vtable;
    };
    template <typename Func, bool Direct = true>
    struct select_vtable_for : direct_vtable_for<Func> {};
    template <typename Func>
    struct select_vtable_for<Func, false> : indirect_vtable_for<Func> {};
    template <typename Func>
    static constexpr bool is_direct() {
        return sizeof(Func) <= nr_direct && alignof(Func) <= alignof(storage)
                && std::is_nothrow_move_constructible<Func>::value;
    }
    template <typename Func>
    struct vtable_for : select_vtable_for<Func, is_direct<Func>()> {};
public:
    noncopyable_function() noexcept : _vtable(&_s_empty_vtable) {}
    template <typename Func>
    noncopyable_function(Func func) {
        vtable_for<Func>::initialize(std::move(func), this);
        _vtable = &vtable_for<Func>::s_vtable;
    }
    template <typename Object, typename... AllButFirstArg>
    noncopyable_function(Ret (Object::*member)(AllButFirstArg...)) : noncopyable_function(std::mem_fn(member)) {}
    template <typename Object, typename... AllButFirstArg>
    noncopyable_function(Ret (Object::*member)(AllButFirstArg...) const) : noncopyable_function(std::mem_fn(member)) {}

    ~noncopyable_function() {
        _vtable->destroy(this);
    }

    noncopyable_function(const noncopyable_function&) = delete;
    noncopyable_function& operator=(const noncopyable_function&) = delete;

    noncopyable_function(noncopyable_function&& x) noexcept : _vtable(std::exchange(x._vtable, &_s_empty_vtable)) {
        _vtable->move(&x, this);
    }

    noncopyable_function& operator=(noncopyable_function&& x) noexcept {
        if (this != &x) {
            this->~noncopyable_function();
            new (this) noncopyable_function(std::move(x));
        }
        return *this;
    }

    Ret operator()(Args... args) const {
        return _vtable->call(this, std::forward<Args>(args)...);
    }

    explicit operator bool() const {
        return _vtable != &_s_empty_vtable;
    }
};


template <typename Ret, typename... Args>
constexpr typename noncopyable_function<Ret (Args...)>::vtable noncopyable_function<Ret (Args...)>::_s_empty_vtable;

template <typename Ret, typename... Args>
template <typename Func>
const typename noncopyable_function<Ret (Args...)>::vtable noncopyable_function<Ret (Args...)>::direct_vtable_for<Func>::s_vtable
        = noncopyable_function<Ret (Args...)>::direct_vtable_for<Func>::make_vtable();


template <typename Ret, typename... Args>
template <typename Func>
const typename noncopyable_function<Ret (Args...)>::vtable noncopyable_function<Ret (Args...)>::indirect_vtable_for<Func>::s_vtable
        = noncopyable_function<Ret (Args...)>::indirect_vtable_for<Func>::make_vtable();

}

