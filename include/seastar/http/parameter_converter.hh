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
 * Copyright (C) 2025 Kefu Chai (tchaikov@gmail.com)
 */

#pragma once

#include <seastar/core/sstring.hh>
#include <seastar/http/parameter_metadata.hh>
#include <seastar/http/parameter_exception.hh>
#include <concepts>
#include <charconv>
#include <string_view>

namespace seastar::httpd {

// C++20 Concepts for parameter type constraints
template<typename T>
concept IntegralParameter = std::integral<T> && !std::same_as<T, bool>;

template<typename T>
concept FloatingPointParameter = std::floating_point<T>;

template<typename T>
concept BooleanParameter = std::same_as<T, bool>;

template<typename T>
concept StringParameter = std::convertible_to<T, sstring>;

// Compile-time type name helper
template<typename T>
consteval const char* type_name() {
    if constexpr (std::same_as<T, int32_t>) {
        return "int32_t";
    } else if constexpr (std::same_as<T, int64_t>) {
        return "int64_t";
    } else if constexpr (std::same_as<T, float>) {
        return "float";
    } else if constexpr (std::same_as<T, double>) {
        return "double";
    } else if constexpr (std::same_as<T, bool>) {
        return "bool";
    } else if constexpr (StringParameter<T>) {
        return "string";
    } else {
        return "unknown";
    }
}

/// Type converter trait for parameter conversion
/// Specializations provide type-specific conversion logic
template<typename T>
struct parameter_converter;

/// Generic implementation for ALL integral types (int32_t, int64_t, etc.)
template<IntegralParameter T>
struct parameter_converter<T> {
    static consteval parameter_type type_enum() {
        if constexpr (sizeof(T) <= 4) {
            return parameter_type::INT32;
        } else {
            return parameter_type::INT64;
        }
    }

    static T convert(std::string_view value) {
        T result;
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);

        // Check for conversion error
        if (ec != std::errc{}) {
            throw type_conversion_exception("", sstring(value), type_name<T>());
        }

        // Check that entire string was consumed
        if (ptr != value.data() + value.size()) {
            throw type_conversion_exception("", sstring(value), type_name<T>());
        }

        return result;
    }
};

/// Generic implementation for ALL floating point types (float, double)
template<FloatingPointParameter T>
struct parameter_converter<T> {
    static consteval parameter_type type_enum() {
        if constexpr (std::same_as<T, float>) {
            return parameter_type::FLOAT;
        } else {
            return parameter_type::DOUBLE;
        }
    }

    static T convert(std::string_view value) {
        T result;
        auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), result);

        // Check for conversion error
        if (ec != std::errc{}) {
            throw type_conversion_exception("", sstring(value), type_name<T>());
        }

        // Check that entire string was consumed
        if (ptr != value.data() + value.size()) {
            throw type_conversion_exception("", sstring(value), type_name<T>());
        }

        return result;
    }
};

/// Specialization for bool
template<BooleanParameter T>
struct parameter_converter<T> {
    static consteval parameter_type type_enum() {
        return parameter_type::BOOLEAN;
    }

    static bool convert(std::string_view value) {
        // Accept various representations of true
        if (value == "true" || value == "1" ||
            value == "True" || value == "TRUE" ||
            value == "yes" || value == "Yes" || value == "YES") {
            return true;
        }

        // Accept various representations of false
        if (value == "false" || value == "0" ||
            value == "False" || value == "FALSE" ||
            value == "no" || value == "No" || value == "NO") {
            return false;
        }

        throw type_conversion_exception("", sstring(value), "bool");
    }
};

/// Specialization for sstring (passthrough)
template<>
struct parameter_converter<sstring> {
    static consteval parameter_type type_enum() {
        return parameter_type::STRING;
    }

    static sstring convert(std::string_view value) {
        return sstring(value);
    }
};



} // namespace seastar::httpd
