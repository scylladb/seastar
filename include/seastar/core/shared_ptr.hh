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

#include <seastar/core/shared_ptr_debug_helper.hh>
#include <seastar/util/is_smart_ptr.hh>
#include <seastar/util/indirect.hh>
#include <seastar/util/modules.hh>
#ifndef SEASTAR_MODULE
#include <boost/intrusive/parent_from_member.hpp>
#include <fmt/core.h>
#include <ostream>
#include <type_traits>
#include <utility>
#endif

#if defined(__GNUC__) && !defined(__clang__) && (__GNUC__ >= 12)
// to silence the false alarm from GCC 12, see
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=105204
#define SEASTAR_IGNORE_USE_AFTER_FREE
#endif

namespace seastar {

// This header defines two shared pointer facilities, lw_shared_ptr<> and
// shared_ptr<>, both modeled after std::shared_ptr<>.
//
// Unlike std::shared_ptr<>, neither of these implementations are thread
// safe, and two pointers sharing the same object must not be used in
// different threads.
//
// lw_shared_ptr<> is the more lightweight variant, with a lw_shared_ptr<>
// occupying just one machine word, and adding just one word to the shared
// object.  However, it does not support polymorphism.
//
// shared_ptr<> is more expensive, with a pointer occupying two machine
// words, and with two words of overhead in the shared object.  In return,
// it does support polymorphism.
//
// Both variants support shared_from_this() via enable_shared_from_this<>
// and lw_enable_shared_from_this<>().
//

SEASTAR_MODULE_EXPORT_BEGIN

#ifndef SEASTAR_DEBUG_SHARED_PTR
using shared_ptr_counter_type = long;
#else
using shared_ptr_counter_type = debug_shared_ptr_counter_type;
#endif

template <typename T>
class lw_shared_ptr;

template <typename T>
class shared_ptr;

template <typename T>
class enable_lw_shared_from_this;

template <typename T>
class enable_shared_from_this;

template <typename T, typename... A>
lw_shared_ptr<T> make_lw_shared(A&&... a);

template <typename T>
lw_shared_ptr<T> make_lw_shared(T&& a);

template <typename T>
lw_shared_ptr<T> make_lw_shared(T& a);

template <typename T, typename... A>
shared_ptr<T> make_shared(A&&... a);

template <typename T>
shared_ptr<T> make_shared(T&& a);

template <typename T, typename U>
shared_ptr<T> static_pointer_cast(const shared_ptr<U>& p);

template <typename T, typename U>
shared_ptr<T> dynamic_pointer_cast(const shared_ptr<U>& p);

template <typename T, typename U>
shared_ptr<T> const_pointer_cast(const shared_ptr<U>& p);

struct lw_shared_ptr_counter_base {
    shared_ptr_counter_type _count = 0;
};

SEASTAR_MODULE_EXPORT_END

namespace internal {

template <class T, class U>
struct lw_shared_ptr_accessors;

template <class T>
struct lw_shared_ptr_accessors_esft;

template <class T>
struct lw_shared_ptr_accessors_no_esft;

}


// We want to support two use cases for shared_ptr<T>:
//
//   1. T is any type (primitive or class type)
//
//   2. T is a class type that inherits from enable_shared_from_this<T>.
//
// In the first case, we must wrap T in an object containing the counter,
// since T may be a primitive type and cannot be a base class.
//
// In the second case, we want T to reach the counter through its
// enable_shared_from_this<> base class, so that we can implement
// shared_from_this().
//
// To implement those two conflicting requirements (T alongside its counter;
// T inherits from an object containing the counter) we use std::conditional<>
// and some accessor functions to select between two implementations.


// CRTP from this to enable shared_from_this:
SEASTAR_MODULE_EXPORT
template <typename T>
class enable_lw_shared_from_this : private lw_shared_ptr_counter_base {
    using ctor = T;
protected:
    enable_lw_shared_from_this() noexcept {}
    enable_lw_shared_from_this(enable_lw_shared_from_this&&) noexcept {}
    enable_lw_shared_from_this(const enable_lw_shared_from_this&) noexcept {}
    enable_lw_shared_from_this& operator=(const enable_lw_shared_from_this&) noexcept { return *this; }
    enable_lw_shared_from_this& operator=(enable_lw_shared_from_this&&) noexcept { return *this; }
public:
    lw_shared_ptr<T> shared_from_this() noexcept;
    lw_shared_ptr<const T> shared_from_this() const noexcept;
    long use_count() const noexcept { return _count; }

    template <typename X>
    friend class lw_shared_ptr;
    template <typename X>
    friend struct internal::lw_shared_ptr_accessors_esft;
    template <typename X, class Y>
    friend struct internal::lw_shared_ptr_accessors;
};

template <typename T>
struct lw_shared_ptr_no_esft : private lw_shared_ptr_counter_base {
    T _value;

    lw_shared_ptr_no_esft() = default;
    lw_shared_ptr_no_esft(const T& x) : _value(x) {}
    lw_shared_ptr_no_esft(T&& x) : _value(std::move(x)) {}
    template <typename... A>
    lw_shared_ptr_no_esft(A&&... a) : _value(std::forward<A>(a)...) {}

    template <typename X>
    friend class lw_shared_ptr;
    template <typename X>
    friend struct internal::lw_shared_ptr_accessors_no_esft;
    template <typename X, class Y>
    friend struct internal::lw_shared_ptr_accessors;
};


/// Extension point: the user may override this to change how \ref lw_shared_ptr objects are destroyed,
/// primarily so that incomplete classes can be used.
///
/// Customizing the deleter requires that \c T be derived from \c enable_lw_shared_from_this<T>.
/// The specialization must be visible for all uses of \c lw_shared_ptr<T>.
///
/// To customize, the template must have a `static void dispose(T*)` operator that disposes of
/// the object.
template <typename T>
struct lw_shared_ptr_deleter;  // No generic implementation

namespace internal {

template <typename T>
struct lw_shared_ptr_accessors_esft {
    using concrete_type = std::remove_const_t<T>;
    static T* to_value(lw_shared_ptr_counter_base* counter) {
        return static_cast<T*>(counter);
    }
    static void dispose(lw_shared_ptr_counter_base* counter) {
        dispose(static_cast<T*>(counter));
    }
    static void dispose(T* value_ptr) {
        delete value_ptr;
    }
    static void instantiate_to_value(lw_shared_ptr_counter_base*) {
        // since to_value() is defined above, we don't need to do anything special
        // to force-instantiate it
    }
};

template <typename T>
struct lw_shared_ptr_accessors_no_esft {
    using concrete_type = lw_shared_ptr_no_esft<T>;
    static T* to_value(lw_shared_ptr_counter_base* counter) {
        return &static_cast<concrete_type*>(counter)->_value;
    }
    static void dispose(lw_shared_ptr_counter_base* counter) {
        delete static_cast<concrete_type*>(counter);
    }
    static void dispose(T* value_ptr) {
        delete boost::intrusive::get_parent_from_member(value_ptr, &concrete_type::_value);
    }
    static void instantiate_to_value(lw_shared_ptr_counter_base*) {
        // since to_value() is defined above, we don't need to do anything special
        // to force-instantiate it
    }
};

// Generic case: lw_shared_ptr_deleter<T> is not specialized, select
// implementation based on whether T inherits from enable_lw_shared_from_this<T>.
template <typename T, typename U = void>
struct lw_shared_ptr_accessors : std::conditional_t<
         std::is_base_of_v<enable_lw_shared_from_this<T>, T>,
         lw_shared_ptr_accessors_esft<T>,
         lw_shared_ptr_accessors_no_esft<T>> {
};

// void_t is C++17, use this temporarily
template <typename... T>
using void_t = void;

// Overload when lw_shared_ptr_deleter<T> specialized
template <typename T>
struct lw_shared_ptr_accessors<T, void_t<decltype(lw_shared_ptr_deleter<T>{})>> {
    using concrete_type = T;
    static T* to_value(lw_shared_ptr_counter_base* counter);
    static void dispose(lw_shared_ptr_counter_base* counter) {
        lw_shared_ptr_deleter<T>::dispose(to_value(counter));
    }
    static void instantiate_to_value(lw_shared_ptr_counter_base* p) {
        // instantiate to_value(); must be defined by shared_ptr_incomplete.hh
        to_value(p);
    }
};

}

SEASTAR_MODULE_EXPORT_BEGIN
template <typename T>
class lw_shared_ptr {
    template <typename U>
    using accessors = internal::lw_shared_ptr_accessors<std::remove_const_t<U>>;

    mutable lw_shared_ptr_counter_base* _p = nullptr;
private:
    lw_shared_ptr(lw_shared_ptr_counter_base* p) noexcept : _p(p) {
        if (_p) {
            ++_p->_count;
        }
    }
    template <typename... A>
    static lw_shared_ptr make(A&&... a) {
        auto p = new typename accessors<T>::concrete_type(std::forward<A>(a)...);
        accessors<T>::instantiate_to_value(p);
        return lw_shared_ptr(p);
    }
public:
    using element_type = T;

    // Destroys the object pointed to by p and disposes of its storage.
    // The pointer to the object must have been obtained through release().
    static void dispose(T* p) noexcept {
        accessors<T>::dispose(const_cast<std::remove_const_t<T>*>(p));
    }

    // A functor which calls dispose().
    class disposer {
    public:
        void operator()(T* p) const noexcept {
            dispose(p);
        }
    };

    lw_shared_ptr() noexcept = default;
    lw_shared_ptr(std::nullptr_t) noexcept : lw_shared_ptr() {}
    lw_shared_ptr(const lw_shared_ptr& x) noexcept : _p(x._p) {
        if (_p) {
#pragma GCC diagnostic push
#ifdef SEASTAR_IGNORE_USE_AFTER_FREE
#pragma GCC diagnostic ignored "-Wuse-after-free"
#endif
            ++_p->_count;
#pragma GCC diagnostic pop
        }
    }
    lw_shared_ptr(lw_shared_ptr&& x) noexcept  : _p(x._p) {
        x._p = nullptr;
    }
    [[gnu::always_inline]]
    ~lw_shared_ptr() {
#pragma GCC diagnostic push
#ifdef SEASTAR_IGNORE_USE_AFTER_FREE
#pragma GCC diagnostic ignored "-Wuse-after-free"
#endif
        if (_p && !--_p->_count) {
            accessors<T>::dispose(_p);
        }
#pragma GCC diagnostic pop
    }
    lw_shared_ptr& operator=(const lw_shared_ptr& x) noexcept {
        if (_p != x._p) {
            this->~lw_shared_ptr();
            new (this) lw_shared_ptr(x);
        }
        return *this;
    }
    lw_shared_ptr& operator=(lw_shared_ptr&& x) noexcept {
        if (_p != x._p) {
            this->~lw_shared_ptr();
            new (this) lw_shared_ptr(std::move(x));
        }
        return *this;
    }
    lw_shared_ptr& operator=(std::nullptr_t) noexcept {
        return *this = lw_shared_ptr();
    }

    T& operator*() const noexcept { return *accessors<T>::to_value(_p); }
    T* operator->() const noexcept { return accessors<T>::to_value(_p); }
    T* get() const noexcept {
        if (_p) {
            return accessors<T>::to_value(_p);
        } else {
            return nullptr;
        }
    }

    // Releases ownership of the object without destroying it.
    // If this was the last owner then returns an engaged unique_ptr
    // which is now the sole owner of the object.
    // Returns a disengaged pointer if there are still some owners.
    //
    // Note that in case the raw pointer is extracted from the unique_ptr
    // using unique_ptr::release(), it must be still destroyed using
    // lw_shared_ptr::disposer or lw_shared_ptr::dispose().
    std::unique_ptr<T, disposer> release() noexcept {
        auto p = std::exchange(_p, nullptr);
        if (--p->_count) {
            return nullptr;
        } else {
            return std::unique_ptr<T, disposer>(accessors<T>::to_value(p));
        }
    }

    long int use_count() const noexcept {
        if (_p) {
            return _p->_count;
        } else {
            return 0;
        }
    }

    operator lw_shared_ptr<const T>() const noexcept {
        return lw_shared_ptr<const T>(_p);
    }

    explicit operator bool() const noexcept {
        return _p;
    }

    bool owned() const noexcept {
        return _p->_count == 1;
    }

    bool operator==(const lw_shared_ptr<const T>& x) const {
        return _p == x._p;
    }

    bool operator!=(const lw_shared_ptr<const T>& x) const {
        return !operator==(x);
    }

    bool operator==(const lw_shared_ptr<std::remove_const_t<T>>& x) const {
        return _p == x._p;
    }

    bool operator!=(const lw_shared_ptr<std::remove_const_t<T>>& x) const {
        return !operator==(x);
    }

    bool operator<(const lw_shared_ptr<const T>& x) const {
        return _p < x._p;
    }

    bool operator<(const lw_shared_ptr<std::remove_const_t<T>>& x) const {
        return _p < x._p;
    }

    template <typename U>
    friend class lw_shared_ptr;

    template <typename X, typename... A>
    friend lw_shared_ptr<X> make_lw_shared(A&&...);

    template <typename U>
    friend lw_shared_ptr<U> make_lw_shared(U&&);

    template <typename U>
    friend lw_shared_ptr<U> make_lw_shared(U&);

    template <typename U>
    friend class enable_lw_shared_from_this;
};

template <typename T, typename... A>
inline
lw_shared_ptr<T> make_lw_shared(A&&... a) {
    return lw_shared_ptr<T>::make(std::forward<A>(a)...);
}

template <typename T>
inline
lw_shared_ptr<T> make_lw_shared(T&& a) {
    return lw_shared_ptr<T>::make(std::move(a));
}

template <typename T>
inline
lw_shared_ptr<T> make_lw_shared(T& a) {
    return lw_shared_ptr<T>::make(a);
}
SEASTAR_MODULE_EXPORT_END

template <typename T>
inline
lw_shared_ptr<T>
enable_lw_shared_from_this<T>::shared_from_this() noexcept {
    return lw_shared_ptr<T>(this);
}

template <typename T>
inline
lw_shared_ptr<const T>
enable_lw_shared_from_this<T>::shared_from_this() const noexcept {
    return lw_shared_ptr<const T>(const_cast<enable_lw_shared_from_this*>(this));
}

SEASTAR_MODULE_EXPORT
template <typename T>
inline
std::ostream& operator<<(std::ostream& out, const lw_shared_ptr<T>& p) {
    if (!p) {
        return out << "null";
    }
    return out << *p;
}

// Polymorphic shared pointer class

struct shared_ptr_count_base {
    // destructor is responsible for fully-typed deletion
    virtual ~shared_ptr_count_base() {}
    shared_ptr_counter_type count = 0;
};

template <typename T>
struct shared_ptr_count_for : shared_ptr_count_base {
    T data;
    template <typename... A>
    shared_ptr_count_for(A&&... a) : data(std::forward<A>(a)...) {}
};

SEASTAR_MODULE_EXPORT_BEGIN
template <typename T>
class enable_shared_from_this : private shared_ptr_count_base {
public:
    shared_ptr<T> shared_from_this() noexcept;
    shared_ptr<const T> shared_from_this() const noexcept;
    long use_count() const noexcept { return count; }

    template <typename U>
    friend class shared_ptr;

    template <typename U, bool esft>
    friend struct shared_ptr_make_helper;
};

template <typename T>
class shared_ptr {
    mutable shared_ptr_count_base* _b = nullptr;
    mutable T* _p = nullptr;
private:
    explicit shared_ptr(shared_ptr_count_for<T>* b) noexcept : _b(b), _p(&b->data) {
        ++_b->count;
    }
    shared_ptr(shared_ptr_count_base* b, T* p) noexcept : _b(b), _p(p) {
        if (_b) {
            ++_b->count;
        }
    }
    explicit shared_ptr(enable_shared_from_this<std::remove_const_t<T>>* p) noexcept : _b(p), _p(static_cast<T*>(p)) {
        if (_b) {
            ++_b->count;
        }
    }
public:
    using element_type = T;

    shared_ptr() noexcept = default;
    shared_ptr(std::nullptr_t) noexcept : shared_ptr() {}
    shared_ptr(const shared_ptr& x) noexcept
            : _b(x._b)
            , _p(x._p) {
        if (_b) {
            ++_b->count;
        }
    }
    shared_ptr(shared_ptr&& x) noexcept
            : _b(x._b)
            , _p(x._p) {
        x._b = nullptr;
        x._p = nullptr;
    }
    template <typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
    shared_ptr(const shared_ptr<U>& x) noexcept
            : _b(x._b)
            , _p(x._p) {
        if (_b) {
            ++_b->count;
        }
    }
    template <typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
    shared_ptr(shared_ptr<U>&& x) noexcept
            : _b(x._b)
            , _p(x._p) {
        x._b = nullptr;
        x._p = nullptr;
    }
    ~shared_ptr() {
#pragma GCC diagnostic push
#ifdef SEASTAR_IGNORE_USE_AFTER_FREE
#pragma GCC diagnostic ignored "-Wuse-after-free"
#endif
        if (_b && !--_b->count) {
            delete _b;
        }
#pragma GCC diagnostic pop
    }
    shared_ptr& operator=(const shared_ptr& x) noexcept {
        if (this != &x) {
            this->~shared_ptr();
            new (this) shared_ptr(x);
        }
        return *this;
    }
    shared_ptr& operator=(shared_ptr&& x) noexcept {
        if (this != &x) {
            this->~shared_ptr();
            new (this) shared_ptr(std::move(x));
        }
        return *this;
    }
    shared_ptr& operator=(std::nullptr_t) noexcept {
        return *this = shared_ptr();
    }
    template <typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
    shared_ptr& operator=(const shared_ptr<U>& x) noexcept {
        if (*this != x) {
            this->~shared_ptr();
            new (this) shared_ptr(x);
        }
        return *this;
    }
    template <typename U, typename = std::enable_if_t<std::is_base_of_v<T, U>>>
    shared_ptr& operator=(shared_ptr<U>&& x) noexcept {
        if (*this != x) {
            this->~shared_ptr();
            new (this) shared_ptr(std::move(x));
        }
        return *this;
    }
    explicit operator bool() const noexcept {
        return _p;
    }
    T& operator*() const noexcept {
        return *_p;
    }
    T* operator->() const noexcept {
        return _p;
    }
    T* get() const noexcept {
        return _p;
    }
    long use_count() const noexcept {
        if (_b) {
            return _b->count;
        } else {
            return 0;
        }
    }

    template <bool esft>
    struct make_helper;

    template <typename U, typename... A>
    friend shared_ptr<U> make_shared(A&&... a);

    template <typename U>
    friend shared_ptr<U> make_shared(U&& a);

    template <typename V, typename U>
    friend shared_ptr<V> static_pointer_cast(const shared_ptr<U>& p);

    template <typename V, typename U>
    friend shared_ptr<V> dynamic_pointer_cast(const shared_ptr<U>& p);

    template <typename V, typename U>
    friend shared_ptr<V> const_pointer_cast(const shared_ptr<U>& p);

    template <bool esft, typename... A>
    static shared_ptr make(A&&... a);

    template <typename U>
    friend class enable_shared_from_this;

    template <typename U, bool esft>
    friend struct shared_ptr_make_helper;

    template <typename U>
    friend class shared_ptr;
};
SEASTAR_MODULE_EXPORT_END

template <typename U, bool esft>
struct shared_ptr_make_helper;

template <typename T>
struct shared_ptr_make_helper<T, false> {
    template <typename... A>
    static shared_ptr<T> make(A&&... a) {
        return shared_ptr<T>(new shared_ptr_count_for<T>(std::forward<A>(a)...));
    }
};

template <typename T>
struct shared_ptr_make_helper<T, true> {
    template <typename... A>
    static shared_ptr<T> make(A&&... a) {
        auto p = new T(std::forward<A>(a)...);
        return shared_ptr<T>(p, p);
    }
};

SEASTAR_MODULE_EXPORT_BEGIN
template <typename T, typename... A>
inline
shared_ptr<T>
make_shared(A&&... a) {
    using helper = shared_ptr_make_helper<T, std::is_base_of_v<shared_ptr_count_base, T>>;
    return helper::make(std::forward<A>(a)...);
}

template <typename T>
inline
shared_ptr<T>
make_shared(T&& a) {
    using helper = shared_ptr_make_helper<T, std::is_base_of_v<shared_ptr_count_base, T>>;
    return helper::make(std::forward<T>(a));
}

template <typename T, typename U>
inline
shared_ptr<T>
static_pointer_cast(const shared_ptr<U>& p) {
    return shared_ptr<T>(p._b, static_cast<T*>(p._p));
}

template <typename T, typename U>
inline
shared_ptr<T>
dynamic_pointer_cast(const shared_ptr<U>& p) {
    auto q = dynamic_cast<T*>(p._p);
    return shared_ptr<T>(q ? p._b : nullptr, q);
}

template <typename T, typename U>
inline
shared_ptr<T>
const_pointer_cast(const shared_ptr<U>& p) {
    return shared_ptr<T>(p._b, const_cast<T*>(p._p));
}
SEASTAR_MODULE_EXPORT_END

template <typename T>
inline
shared_ptr<T>
enable_shared_from_this<T>::shared_from_this() noexcept {
    auto unconst = reinterpret_cast<enable_shared_from_this<std::remove_const_t<T>>*>(this);
    return shared_ptr<T>(unconst);
}

template <typename T>
inline
shared_ptr<const T>
enable_shared_from_this<T>::shared_from_this() const noexcept {
    auto esft = const_cast<enable_shared_from_this*>(this);
    auto unconst = reinterpret_cast<enable_shared_from_this<std::remove_const_t<T>>*>(esft);
    return shared_ptr<const T>(unconst);
}

SEASTAR_MODULE_EXPORT_BEGIN
template <typename T, typename U>
inline
bool
operator==(const shared_ptr<T>& x, const shared_ptr<U>& y) {
    return x.get() == y.get();
}

template <typename T>
inline
bool
operator==(const shared_ptr<T>& x, std::nullptr_t) {
    return x.get() == nullptr;
}

template <typename T>
inline
bool
operator==(std::nullptr_t, const shared_ptr<T>& y) {
    return nullptr == y.get();
}

template <typename T>
inline
bool
operator==(const lw_shared_ptr<T>& x, std::nullptr_t) {
    return x.get() == nullptr;
}

template <typename T>
inline
bool
operator==(std::nullptr_t, const lw_shared_ptr<T>& y) {
    return nullptr == y.get();
}

template <typename T, typename U>
inline
bool
operator!=(const shared_ptr<T>& x, const shared_ptr<U>& y) {
    return x.get() != y.get();
}

template <typename T>
inline
bool
operator!=(const shared_ptr<T>& x, std::nullptr_t) {
    return x.get() != nullptr;
}

template <typename T>
inline
bool
operator!=(std::nullptr_t, const shared_ptr<T>& y) {
    return nullptr != y.get();
}

template <typename T>
inline
bool
operator!=(const lw_shared_ptr<T>& x, std::nullptr_t) {
    return x.get() != nullptr;
}

template <typename T>
inline
bool
operator!=(std::nullptr_t, const lw_shared_ptr<T>& y) {
    return nullptr != y.get();
}

template <typename T, typename U>
inline
bool
operator<(const shared_ptr<T>& x, const shared_ptr<U>& y) {
    return x.get() < y.get();
}

template <typename T>
inline
bool
operator<(const shared_ptr<T>& x, std::nullptr_t) {
    return x.get() < nullptr;
}

template <typename T>
inline
bool
operator<(std::nullptr_t, const shared_ptr<T>& y) {
    return nullptr < y.get();
}

template <typename T, typename U>
inline
bool
operator<=(const shared_ptr<T>& x, const shared_ptr<U>& y) {
    return x.get() <= y.get();
}

template <typename T>
inline
bool
operator<=(const shared_ptr<T>& x, std::nullptr_t) {
    return x.get() <= nullptr;
}

template <typename T>
inline
bool
operator<=(std::nullptr_t, const shared_ptr<T>& y) {
    return nullptr <= y.get();
}

template <typename T, typename U>
inline
bool
operator>(const shared_ptr<T>& x, const shared_ptr<U>& y) {
    return x.get() > y.get();
}

template <typename T>
inline
bool
operator>(const shared_ptr<T>& x, std::nullptr_t) {
    return x.get() > nullptr;
}

template <typename T>
inline
bool
operator>(std::nullptr_t, const shared_ptr<T>& y) {
    return nullptr > y.get();
}

template <typename T, typename U>
inline
bool
operator>=(const shared_ptr<T>& x, const shared_ptr<U>& y) {
    return x.get() >= y.get();
}

template <typename T>
inline
bool
operator>=(const shared_ptr<T>& x, std::nullptr_t) {
    return x.get() >= nullptr;
}

template <typename T>
inline
bool
operator>=(std::nullptr_t, const shared_ptr<T>& y) {
    return nullptr >= y.get();
}

template <typename T>
inline
std::ostream& operator<<(std::ostream& out, const shared_ptr<T>& p) {
    if (!p) {
        return out << "null";
    }
    return out << *p;
}

template<typename T>
using shared_ptr_equal_by_value = indirect_equal_to<shared_ptr<T>>;

template<typename T>
using shared_ptr_value_hash = indirect_hash<shared_ptr<T>>;

SEASTAR_MODULE_EXPORT_END
}

namespace std {

SEASTAR_MODULE_EXPORT
template <typename T>
struct hash<seastar::lw_shared_ptr<T>> : private hash<T*> {
    size_t operator()(const seastar::lw_shared_ptr<T>& p) const {
        return hash<T*>::operator()(p.get());
    }
};

SEASTAR_MODULE_EXPORT
template <typename T>
struct hash<seastar::shared_ptr<T>> : private hash<T*> {
    size_t operator()(const seastar::shared_ptr<T>& p) const {
        return hash<T*>::operator()(p.get());
    }
};

}

SEASTAR_MODULE_EXPORT
namespace fmt {

template<typename T>
const void* ptr(const seastar::lw_shared_ptr<T>& p) {
    return p.get();
}

template<typename T>
const void* ptr(const seastar::shared_ptr<T>& p) {
    return p.get();
}

template <typename T>
struct formatter<seastar::shared_ptr<T>> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    auto format(const seastar::shared_ptr<T>& p, fmt::format_context& ctx) const {
        if (!p) {
            return fmt::format_to(ctx.out(), "null");
        }
        return fmt::format_to(ctx.out(), "{}", *p);
    }
};

template <typename T>
struct formatter<seastar::lw_shared_ptr<T>> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    auto format(const seastar::lw_shared_ptr<T>& p, fmt::format_context& ctx) const {
        if (!p) {
            return fmt::format_to(ctx.out(), "null");
        }
        return fmt::format_to(ctx.out(), "{}", *p);
    }
};
}

namespace seastar {

SEASTAR_MODULE_EXPORT
template<typename T>
struct is_smart_ptr<shared_ptr<T>> : std::true_type {};

SEASTAR_MODULE_EXPORT
template<typename T>
struct is_smart_ptr<lw_shared_ptr<T>> : std::true_type {};

}
