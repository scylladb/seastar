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
#include <seastar/http/parameter_exception.hh>
#include <memory>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstdint>

namespace seastar::httpd {


/// Enumeration of supported parameter types
enum class parameter_type {
    STRING,
    INT32,
    INT64,
    FLOAT,
    DOUBLE,
    BOOLEAN,
    ENUM
};

/// Enumeration of parameter locations
enum class parameter_location {
    QUERY,  ///< Query string parameter
    PATH    ///< Path parameter
};

/// Base class for parameter constraints
class parameter_constraint {
public:
    virtual ~parameter_constraint() = default;

    /// Validate a parameter value against this constraint
    /// Throws an appropriate exception if validation fails
    virtual void validate(const sstring& param_name, const sstring& value) const = 0;

    /// Create a deep copy of this constraint
    virtual std::unique_ptr<parameter_constraint> clone() const = 0;
};

/// Constraint for numeric range validation (min/max)
class range_constraint : public parameter_constraint {
    std::optional<int64_t> _min;
    std::optional<int64_t> _max;

public:
    range_constraint(std::optional<int64_t> min, std::optional<int64_t> max)
        : _min(min), _max(max) {}

    void validate(const sstring& param_name, const sstring& value) const override;
    std::unique_ptr<parameter_constraint> clone() const override;

    constexpr const std::optional<int64_t>& min() const noexcept { return _min; }
    constexpr const std::optional<int64_t>& max() const noexcept { return _max; }
};

/// Constraint for string length validation (minLength/maxLength)
class length_constraint : public parameter_constraint {
    std::optional<size_t> _min_length;
    std::optional<size_t> _max_length;

public:
    length_constraint(std::optional<size_t> min_length,
                     std::optional<size_t> max_length)
        : _min_length(min_length), _max_length(max_length) {}

    void validate(const sstring& param_name, const sstring& value) const override;
    std::unique_ptr<parameter_constraint> clone() const override;

    constexpr const std::optional<size_t>& min_length() const noexcept { return _min_length; }
    constexpr const std::optional<size_t>& max_length() const noexcept { return _max_length; }
};

/// Constraint for enum validation (allowed values)
class enum_constraint : public parameter_constraint {
    std::vector<sstring> _allowed_values;

public:
    enum_constraint(std::vector<sstring> allowed_values)
        : _allowed_values(std::move(allowed_values)) {}

    void validate(const sstring& param_name, const sstring& value) const override;
    std::unique_ptr<parameter_constraint> clone() const override;

    constexpr const std::vector<sstring>& allowed_values() const noexcept {
        return _allowed_values;
    }
};

/// Metadata describing a single parameter's type and constraints
class parameter_metadata {
    sstring _name;
    parameter_type _type;
    parameter_location _location;
    bool _required;
    std::vector<std::unique_ptr<parameter_constraint>> _constraints;
    std::optional<sstring> _default_value;

public:
    parameter_metadata(sstring name,
                      parameter_type type,
                      parameter_location location,
                      bool required = false)
        : _name(std::move(name))
        , _type(type)
        , _location(location)
        , _required(required) {}

    // Copy constructor - deep copy constraints
    parameter_metadata(const parameter_metadata& other);

    // Copy assignment
    parameter_metadata& operator=(const parameter_metadata& other);

    // Move constructor and assignment
    parameter_metadata(parameter_metadata&&) noexcept = default;
    parameter_metadata& operator=(parameter_metadata&&) noexcept = default;

    /// Add a range constraint (min/max for numeric types)
    /// Returns *this for method chaining
    parameter_metadata& with_range(std::optional<int64_t> min,
                                   std::optional<int64_t> max);

    /// Add a length constraint (minLength/maxLength for strings)
    /// Returns *this for method chaining
    parameter_metadata& with_length(std::optional<size_t> min_length,
                                    std::optional<size_t> max_length);

    /// Add an enum constraint (allowed values)
    /// Returns *this for method chaining
    parameter_metadata& with_enum(std::vector<sstring> allowed_values);

    /// Set a default value
    /// Returns *this for method chaining
    parameter_metadata& with_default(sstring default_value);

    /// Validate a value against all constraints
    /// Throws an appropriate exception if validation fails
    void validate(const sstring& value) const;

    // Accessors
    constexpr const sstring& name() const noexcept { return _name; }
    constexpr parameter_type type() const noexcept { return _type; }
    constexpr parameter_location location() const noexcept { return _location; }
    constexpr bool required() const noexcept { return _required; }
    constexpr const std::optional<sstring>& default_value() const noexcept {
        return _default_value;
    }
};

/// Registry of parameter metadata for a route
/// Stores and provides access to metadata for all parameters of a route
class parameter_metadata_registry {
    std::unordered_map<sstring, parameter_metadata> _params;

public:
    /// Register metadata for a parameter
    void register_parameter(parameter_metadata metadata);

    /// Get metadata for a parameter by name
    /// Returns nullptr if no metadata exists for the parameter
    const parameter_metadata* get_metadata(const sstring& name) const;

    /// Check if metadata exists for a parameter
    bool has_metadata(const sstring& name) const;

    /// Get all registered parameter metadata
    constexpr const std::unordered_map<sstring, parameter_metadata>& all_params() const noexcept {
        return _params;
    }
};



} // namespace seastar::httpd
