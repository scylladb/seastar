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

namespace seastar {

/// \addtogroup utilities
/// @{

/// Wrapper for lvalue references
///
/// reference_wrapper wraps a lvalue reference into a copyable and assignable
/// object. It is very similar to std::reference_wrapper except that it doesn't
/// allow implicit construction from a reference and the only way to construct
/// it is to use either ref() or cref(). The reason for that discrepancy (and
/// also the reason why seastar::reference_wrapper was introduced) is that it
/// server different purpose than std::reference_wrapper. The latter protects
/// references from decaying and allows copying and assigning them.
/// seastar::reference_wrapper is used mainly to force user to explicitly
/// state that object is passed by reference thus reducing the chances that
/// the referred object being prematurely destroyed in case the execution
/// is deferred to a continuation.
template<typename T>
class reference_wrapper {
    T* _pointer;

    explicit reference_wrapper(T& object) noexcept : _pointer(&object) { }

    template<typename U>
    friend reference_wrapper<U> ref(U&) noexcept;
    template<typename U>
    friend reference_wrapper<const U> cref(const U&) noexcept;
public:
    using type = T;

    operator T&() const noexcept { return *_pointer; }
    T& get() const noexcept { return *_pointer; }

};

/// Wraps reference in a reference_wrapper
template<typename T>
inline reference_wrapper<T> ref(T& object) noexcept {
    return reference_wrapper<T>(object);
}

/// Wraps constant reference in a reference_wrapper
template<typename T>
inline reference_wrapper<const T> cref(const T& object) noexcept {
    return reference_wrapper<const T>(object);
}

/// @}

}
