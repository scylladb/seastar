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
 * Copyright (C) 2026 ScyllaDB
 */

#pragma once

#include <string_view>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <ostream>
#include <type_traits>

namespace seastar::testing {

/// A wrapper that makes any fmt-formattable type printable via operator<<,
/// while forwarding all comparisons to the underlying value.
///
/// This bridges the gap between Seastar's move away from operator<< (in favor
/// of fmt::format) and Boost.Test's reliance on operator<< for failure messages.
template <typename T>
class comparable_formattable {
    // Store const char* as std::string_view to compare by string value rather than pointer value.
    using ref_type = std::conditional_t<std::is_same_v<T, const char*>, std::string_view, const T&>;
    ref_type _value;
public:
    explicit comparable_formattable(const T& value) noexcept : _value(value) {}

    ref_type get() const noexcept { return _value; }

    friend std::ostream& operator<<(std::ostream& os, const comparable_formattable& cf) {
        if constexpr (fmt::is_formattable<T>::value) {
            fmt::print(os, "{}", cf._value);
        } else if constexpr (std::is_pointer_v<T>) {
            fmt::print(os, "{}", fmt::ptr(cf._value));
        } else if constexpr (requires(std::ostream& o, const T& v) { o << v; }) {
            os << cf._value;
        } else {
            static_assert(fmt::is_formattable<T>::value,
                "Type is not fmt-formattable; use BOOST_REQUIRE/BOOST_CHECK directly");
        }
        return os;
    }

    template <typename U>
    friend bool operator==(const comparable_formattable& a, const comparable_formattable<U>& b) {
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wsign-compare"
        return a._value == b.get();
        #pragma GCC diagnostic pop
    }

    template <typename U>
    friend bool operator!=(const comparable_formattable& a, const comparable_formattable<U>& b) {
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wsign-compare"
        return a._value != b.get();
        #pragma GCC diagnostic pop
    }

    template <typename U>
    friend bool operator<(const comparable_formattable& a, const comparable_formattable<U>& b) {
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wsign-compare"
        return a._value < b.get();
        #pragma GCC diagnostic pop
    }

    template <typename U>
    friend bool operator<=(const comparable_formattable& a, const comparable_formattable<U>& b) {
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wsign-compare"
        return a._value <= b.get();
        #pragma GCC diagnostic pop
    }

    template <typename U>
    friend bool operator>(const comparable_formattable& a, const comparable_formattable<U>& b) {
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wsign-compare"
        return a._value > b.get();
        #pragma GCC diagnostic pop
    }

    template <typename U>
    friend bool operator>=(const comparable_formattable& a, const comparable_formattable<U>& b) {
        #pragma GCC diagnostic push
        #pragma GCC diagnostic ignored "-Wsign-compare"
        return a._value >= b.get();
        #pragma GCC diagnostic pop
    }
};

} // namespace seastar::testing

// Wrapper macros that use fmt for formatting instead of operator<<.
// Use these in place of BOOST_REQUIRE_EQUAL, BOOST_CHECK_EQUAL, etc.
// when the types involved have fmt::formatter but not operator<<.

#define SEASTAR_BOOST_REQUIRE_EQUAL(a, b) \
    BOOST_REQUIRE_EQUAL(::seastar::testing::comparable_formattable(a), \
                        ::seastar::testing::comparable_formattable(b))

#define SEASTAR_BOOST_CHECK_EQUAL(a, b) \
    BOOST_CHECK_EQUAL(::seastar::testing::comparable_formattable(a), \
                      ::seastar::testing::comparable_formattable(b))

#define SEASTAR_BOOST_REQUIRE_NE(a, b) \
    BOOST_REQUIRE_NE(::seastar::testing::comparable_formattable(a), \
                     ::seastar::testing::comparable_formattable(b))

#define SEASTAR_BOOST_CHECK_NE(a, b) \
    BOOST_CHECK_NE(::seastar::testing::comparable_formattable(a), \
                   ::seastar::testing::comparable_formattable(b))

#define SEASTAR_BOOST_REQUIRE_LT(a, b) \
    BOOST_REQUIRE_LT(::seastar::testing::comparable_formattable(a), \
                     ::seastar::testing::comparable_formattable(b))

#define SEASTAR_BOOST_CHECK_LT(a, b) \
    BOOST_CHECK_LT(::seastar::testing::comparable_formattable(a), \
                   ::seastar::testing::comparable_formattable(b))

#define SEASTAR_BOOST_REQUIRE_LE(a, b) \
    BOOST_REQUIRE_LE(::seastar::testing::comparable_formattable(a), \
                     ::seastar::testing::comparable_formattable(b))

#define SEASTAR_BOOST_CHECK_LE(a, b) \
    BOOST_CHECK_LE(::seastar::testing::comparable_formattable(a), \
                   ::seastar::testing::comparable_formattable(b))

#define SEASTAR_BOOST_REQUIRE_GT(a, b) \
    BOOST_REQUIRE_GT(::seastar::testing::comparable_formattable(a), \
                     ::seastar::testing::comparable_formattable(b))

#define SEASTAR_BOOST_CHECK_GT(a, b) \
    BOOST_CHECK_GT(::seastar::testing::comparable_formattable(a), \
                   ::seastar::testing::comparable_formattable(b))

#define SEASTAR_BOOST_REQUIRE_GE(a, b) \
    BOOST_REQUIRE_GE(::seastar::testing::comparable_formattable(a), \
                     ::seastar::testing::comparable_formattable(b))

#define SEASTAR_BOOST_CHECK_GE(a, b) \
    BOOST_CHECK_GE(::seastar::testing::comparable_formattable(a), \
                   ::seastar::testing::comparable_formattable(b))
