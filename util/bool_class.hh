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
 * Copyright (C) 2016 ScyllaDB.
 */

#pragma once

#include <ostream>

namespace seastar {

/// \addtogroup utilities
/// @{

/// \brief Type-safe boolean
///
/// bool_class objects are type-safe boolean values that cannot be implicitly
/// casted to untyped bools, integers or different bool_class types while still
/// provides all relevant logical and comparison operators.
///
/// bool_class template parameter is a tag type that is going to be used to
/// distinguish booleans of different types.
///
/// Usage examples:
/// \code
/// struct foo_tag { };
/// using foo = bool_class<foo_tag>;
///
/// struct bar_tag { };
/// using bar = bool_class<bar_tag>;
///
/// foo v1 = foo::yes; // OK
/// bar v2 = foo::yes; // ERROR, no implicit cast
/// foo v4 = v1 || foo::no; // OK
/// bar v5 = bar::yes && bar(true); // OK
/// bool v6 = v5; // ERROR, no implicit cast
/// \endcode
///
/// \tparam Tag type used as a tag
template<typename Tag>
class bool_class {
    bool _value;
public:
    static const bool_class yes;
    static const bool_class no;

    /// Constructs a bool_class object initialised to \c false.
    constexpr bool_class() noexcept : _value(false) { }

    /// Constructs a bool_class object initialised to \c v.
    constexpr explicit bool_class(bool v) noexcept : _value(v) { }

    /// Casts a bool_class object to an untyped \c bool.
    explicit operator bool() const noexcept { return _value; }

    /// Logical OR.
    friend bool_class operator||(bool_class x, bool_class y) noexcept {
        return bool_class(x._value || y._value);
    }

    /// Logical AND.
    friend bool_class operator&&(bool_class x, bool_class y) noexcept {
        return bool_class(x._value && y._value);
    }

    /// Logical NOT.
    friend bool_class operator!(bool_class x) noexcept {
        return bool_class(!x._value);
    }

    /// Equal-to operator.
    friend bool operator==(bool_class x, bool_class y) noexcept {
        return x._value == y._value;
    }

    /// Not-equal-to operator.
    friend bool operator!=(bool_class x, bool_class y) noexcept {
        return x._value != y._value;
    }

    /// Prints bool_class value to an output stream.
    friend std::ostream& operator<<(std::ostream& os, bool_class v) {
        return os << (v._value ? "true" : "false");
    }
};

template<typename Tag>
const bool_class<Tag> bool_class<Tag>::yes { true };
template<typename Tag>
const bool_class<Tag> bool_class<Tag>::no { false };

/// @}

}
