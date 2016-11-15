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

#include <boost/intrusive/list.hpp>

template<typename T>
class weak_ptr {
    template<typename U>
    friend class weakly_referencable;
private:
    using hook_type = boost::intrusive::list_member_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;
    hook_type _hook;
    T* _ptr = nullptr;
    weak_ptr(T* p) : _ptr(p) {}
public:
    weak_ptr() = default;
    weak_ptr(weak_ptr&& o) noexcept
        : _ptr(o._ptr)
    {
        _hook.swap_nodes(o._hook);
        o._ptr = nullptr;
    }
    weak_ptr& operator=(weak_ptr&& o) noexcept {
        if (this != &o) {
            this->~weak_ptr();
            new (this) weak_ptr(std::move(o));
        }
        return *this;
    }
    explicit operator bool() const { return _ptr != nullptr; }
    T* operator->() noexcept { return _ptr; }
    const T* operator->() const noexcept { return _ptr; }
    T& operator*() noexcept { return *_ptr; }
    const T& operator*() const noexcept { return *_ptr; }
    T* get() noexcept { return _ptr; }
    const T* get() const noexcept { return _ptr; }
    bool operator==(weak_ptr& o) const { return _ptr == o._ptr; }
    bool operator!=(weak_ptr& o) const { return _ptr != o._ptr; }
};

/// Allows obtaining a non-owning reference (weak_ptr<>) to the object.
/// A live weak_ptr<> object doesn't prevent the referenced object form being destroyed.
///
/// The underlying pointer held by weak_ptr<> is valid as long as the referenced object is alive.
/// When the object dies, all weak_ptr objects associated with it are emptied.
///
/// A weak reference is obtained like this:
///
///     class X : public weakly_referencable<X> {};
///     auto x = std::make_unique<X>();
///     weak_ptr<X> ptr = x->weak_from_this();
///
/// The user of weak_ptr can check if it still holds a valid pointer like this:
///
///     if (ptr) ptr->do_something();
///
template<typename T>
class weakly_referencable {
    boost::intrusive::list<weak_ptr<T>,
            boost::intrusive::member_hook<weak_ptr<T>, typename weak_ptr<T>::hook_type, &weak_ptr<T>::_hook>,
            boost::intrusive::constant_time_size<false>> _ptr_list;
public:
    weakly_referencable() = default;
    weakly_referencable(weakly_referencable&&) = delete; // pointer to this is captured and passed to weak_ptr
    weakly_referencable(const weakly_referencable&) = delete;
    ~weakly_referencable() noexcept {
        _ptr_list.clear_and_dispose([] (weak_ptr<T>* wp) noexcept {
            wp->_ptr = nullptr;
        });
    }
    weak_ptr<T> weak_from_this() noexcept {
        weak_ptr<T> ptr(static_cast<T*>(this));
        _ptr_list.push_back(ptr);
        return ptr;
    }
};
