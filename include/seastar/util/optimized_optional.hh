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

#include <seastar/util/concepts.hh>
#include <seastar/util/std-compat.hh>

#include <iostream>
#include <memory>
#include <type_traits>

namespace seastar {

SEASTAR_CONCEPT(

template<typename T>
concept OptimizableOptional =
    std::is_default_constructible<T>::value
        && std::is_nothrow_move_assignable<T>::value
        && requires(const T& obj) {
            { bool(obj) } noexcept;
        };

)

/// \c optimized_optional<> is intended mainly for use with classes that store
/// their data externally and expect pointer to this data to be always non-null.
/// In such case there is no real need for another flag signifying whether
/// the optional is engaged.
template<typename T>
class optimized_optional {
    T _object;
public:
    optimized_optional() = default;
    optimized_optional(std::nullopt_t) noexcept { }
    optimized_optional(const T& obj) : _object(obj) { }
    optimized_optional(T&& obj) noexcept : _object(std::move(obj)) { }
    optimized_optional(std::optional<T>&& obj) noexcept {
        if (obj) {
            _object = std::move(*obj);
        }
    }
    optimized_optional(const optimized_optional&) = default;
    optimized_optional(optimized_optional&&) = default;

    optimized_optional& operator=(std::nullopt_t) noexcept {
        _object = T();
        return *this;
    }
    template<typename U>
    std::enable_if_t<std::is_same<std::decay_t<U>, T>::value, optimized_optional&>
    operator=(U&& obj) noexcept {
        _object = std::forward<U>(obj);
        return *this;
    }
    optimized_optional& operator=(const optimized_optional&) = default;
    optimized_optional& operator=(optimized_optional&&) = default;

    explicit operator bool() const noexcept {
        return bool(_object);
    }

    T* operator->() noexcept { return &_object; }
    const T* operator->() const noexcept { return &_object; }

    T& operator*() noexcept { return _object; }
    const T& operator*() const noexcept { return _object; }

    bool operator==(const optimized_optional& other) const {
        return _object == other._object;
    }
    bool operator!=(const optimized_optional& other) const {
        return _object != other._object;
    }
    friend std::ostream& operator<<(std::ostream& out, const optimized_optional& opt) {
        if (!opt) {
            return out << "null";
        }
        return out << *opt;
    }
};

}
