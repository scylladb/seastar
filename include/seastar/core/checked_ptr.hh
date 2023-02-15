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
 * Copyright (C) 2017 ScyllaDB
 */

#pragma once

/// \file
/// \brief Contains a seastar::checked_ptr class implementation.

#include <exception>
#include <memory>
#include <seastar/util/concepts.hh>

/// \namespace seastar
namespace seastar {

/// The exception thrown by a default_null_deref_action.
class checked_ptr_is_null_exception : public std::exception {};

/// \brief
/// Default not engaged seastar::checked_ptr dereferencing action (functor).
///
/// Throws a seastar::checked_ptr_is_null_exception.
///
struct default_null_deref_action {
    /// \throw seastar::checked_ptr_is_null_exception
    void operator()() const {
        throw checked_ptr_is_null_exception();
    }
};

/// \cond internal
/// \namespace seastar::internal
namespace internal {

/// \name seastar::checked_ptr::get() helpers
/// Helper functions that simplify the seastar::checked_ptr::get() implementation.
/// @{

/// Invokes the get() method of a smart pointer object.
/// \param ptr A smart pointer object
/// \return A pointer to the underlying object
template <typename T>
/// cond SEASTAR_CONCEPT_DOC - nested '\ cond' doesn't seem to work (bug 736553), so working it around
SEASTAR_CONCEPT( requires requires (T ptr) {
    ptr.get();
})
/// endcond
inline typename std::pointer_traits<std::remove_const_t<T>>::element_type* checked_ptr_do_get(T& ptr) {
    return ptr.get();
}

/// Return a pointer itself for a naked pointer argument.
/// \param ptr A naked pointer object
/// \return An input naked pointer object
template <typename T>
inline T* checked_ptr_do_get(T* ptr) noexcept {
    return ptr;
}
/// @}
}
/// \endcond

/// \class seastar::checked_ptr
/// \brief
/// seastar::checked_ptr class is a wrapper class that may be used with any pointer type
/// (smart like std::unique_ptr or raw pointers like int*).
///
/// The seastar::checked_ptr object will invoke the NullDerefAction functor if
/// it is dereferenced when the underlying pointer is not engaged.
///
/// It may still be assigned, compared to other seastar::checked_ptr objects or
/// moved without limitations.
///
/// The default NullDerefAction will throw a  seastar::default_null_deref_action exception.
///
/// \tparam NullDerefAction a functor that is invoked when a user tries to dereference a not engaged pointer.
///
template<typename Ptr, typename NullDerefAction = default_null_deref_action>
/// \cond SEASTAR_CONCEPT_DOC
SEASTAR_CONCEPT( requires std::is_default_constructible<NullDerefAction>::value && requires (NullDerefAction action) {
    NullDerefAction();
})
/// \endcond
class checked_ptr {
public:
    /// Underlying element type
    using element_type = typename std::pointer_traits<Ptr>::element_type;

    /// Type of the pointer to the underlying element
    using pointer = element_type*;

private:
    Ptr _ptr = nullptr;

private:
    /// Invokes a NullDerefAction functor if the underlying pointer is not engaged.
    void check() const {
        if (!_ptr) {
            NullDerefAction()();
        }
    }

public:
    checked_ptr() noexcept(noexcept(Ptr(nullptr))) = default;
    checked_ptr(std::nullptr_t) noexcept(std::is_nothrow_default_constructible<checked_ptr<Ptr, NullDerefAction>>::value) : checked_ptr() {}
    checked_ptr(Ptr&& ptr) noexcept(std::is_nothrow_move_constructible<Ptr>::value) : _ptr(std::move(ptr)) {}
    checked_ptr(const Ptr& p) noexcept(std::is_nothrow_copy_constructible<Ptr>::value) : _ptr(p) {}

    /// \name Checked Methods
    /// These methods start with invoking a NullDerefAction functor if the underlying pointer is not engaged.
    /// @{

    /// Invokes the get() method of the underlying smart pointer or returns the pointer itself for a raw pointer (const variant).
    /// \return The pointer to the underlying object
    pointer get() const {
        check();
        return internal::checked_ptr_do_get(_ptr);
    }

    /// Gets a reference to the underlying pointer object.
    /// \return The underlying pointer object
    const Ptr& operator->() const {
        check();
        return _ptr;
    }

    /// Gets a reference to the underlying pointer object (const variant).
    /// \return The underlying pointer object
    Ptr& operator->() {
        check();
        return _ptr;
    }

    /// Gets the reference to the underlying object (const variant).
    /// \return The reference to the underlying object
    const element_type& operator*() const {
        check();
        return *_ptr;
    }

    /// Gets the reference to the underlying object.
    /// \return The reference to the underlying object
    element_type& operator*() {
        check();
        return *_ptr;
    }
    /// @}

    /// \name Unchecked methods
    /// These methods may be invoked when the underlying pointer is not engaged.
    /// @{

    /// Checks if the underlying pointer is engaged.
    /// \return TRUE if the underlying pointer is engaged
    explicit operator bool() const { return bool(_ptr); }

    bool operator==(const checked_ptr& other) const { return _ptr == other._ptr; }
    bool operator!=(const checked_ptr& other) const { return _ptr != other._ptr; }

    /// Gets the hash value for the underlying pointer object.
    /// \return The hash value for the underlying pointer object
    size_t hash() const {
        return std::hash<Ptr>()(_ptr);
    }
    ///@}
};

}

namespace std {
/// std::hash specialization for seastar::checked_ptr class
template<typename T>
struct hash<seastar::checked_ptr<T>> {
    /// Get the hash value for the given seastar::checked_ptr object.
    /// The hash will calculated using the seastar::checked_ptr::hash method.
    /// \param p object for hash value calculation
    /// \return The hash value for the given object
    size_t operator()(const seastar::checked_ptr<T>& p) const {
        return p.hash();
    }
};
}
