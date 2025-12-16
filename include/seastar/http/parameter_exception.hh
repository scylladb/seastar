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

#include <seastar/http/exception.hh>
#include <seastar/core/sstring.hh>
#include <optional>
#include <vector>
#include <cstdint>

namespace seastar::httpd {

/// Base class for parameter validation exceptions
/// Provides detailed information about which parameter failed validation
class parameter_validation_exception : public bad_param_exception {
protected:
    sstring _param_name;
    sstring _param_value;

    static std::string format_message(const sstring& param_name,
                                     const sstring& param_value,
                                     const sstring& reason);

public:
    parameter_validation_exception(const sstring& param_name,
                                  const sstring& param_value,
                                  const sstring& reason)
        : bad_param_exception(format_message(param_name, param_value, reason))
        , _param_name(param_name)
        , _param_value(param_value) {}

    /// Get the name of the parameter that failed validation
    const sstring& param_name() const noexcept { return _param_name; }

    /// Get the value that failed validation
    const sstring& param_value() const noexcept { return _param_value; }
};

/// Exception thrown when type conversion fails
/// Example: Converting "abc" to an integer
class type_conversion_exception : public parameter_validation_exception {
    sstring _target_type;

public:
    type_conversion_exception(const sstring& param_name,
                            const sstring& param_value,
                            const sstring& target_type);

    /// Get the type that conversion was attempted to
    const sstring& target_type() const noexcept { return _target_type; }
};

/// Base class for constraint violation exceptions
class constraint_violation_exception : public parameter_validation_exception {
    sstring _constraint_description;

public:
    constraint_violation_exception(const sstring& param_name,
                                 const sstring& param_value,
                                 const sstring& constraint_description);

    /// Get a description of the constraint that was violated
    const sstring& constraint_description() const noexcept {
        return _constraint_description;
    }
};

/// Exception thrown when a numeric value is out of range
class range_constraint_exception : public constraint_violation_exception {
    std::optional<int64_t> _min;
    std::optional<int64_t> _max;

public:
    range_constraint_exception(const sstring& param_name,
                             const sstring& param_value,
                             std::optional<int64_t> min,
                             std::optional<int64_t> max);

    /// Get the minimum allowed value (if specified)
    const std::optional<int64_t>& min() const noexcept { return _min; }

    /// Get the maximum allowed value (if specified)
    const std::optional<int64_t>& max() const noexcept { return _max; }
};

/// Exception thrown when a string length constraint is violated
class length_constraint_exception : public constraint_violation_exception {
    std::optional<size_t> _min_length;
    std::optional<size_t> _max_length;

public:
    length_constraint_exception(const sstring& param_name,
                               const sstring& param_value,
                               std::optional<size_t> min_length,
                               std::optional<size_t> max_length);

    /// Get the minimum allowed length (if specified)
    const std::optional<size_t>& min_length() const noexcept { return _min_length; }

    /// Get the maximum allowed length (if specified)
    const std::optional<size_t>& max_length() const noexcept { return _max_length; }
};

/// Exception thrown when a value is not in the allowed enum values
class enum_constraint_exception : public constraint_violation_exception {
    std::vector<sstring> _allowed_values;

public:
    enum_constraint_exception(const sstring& param_name,
                            const sstring& param_value,
                            const std::vector<sstring>& allowed_values);

    /// Get the list of allowed values
    const std::vector<sstring>& allowed_values() const noexcept {
        return _allowed_values;
    }
};

} // namespace seastar::httpd
