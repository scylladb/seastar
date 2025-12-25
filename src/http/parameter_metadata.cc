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

#include <seastar/http/parameter_metadata.hh>
#include <algorithm>
#include <ranges>

namespace seastar::httpd {


// range_constraint implementation

void range_constraint::validate(const sstring& param_name, const sstring& value) const {
    try {
        int64_t num_value = std::stoll(std::string(value));

        if (_min && num_value < *_min) {
            throw range_constraint_exception(param_name, value, _min, _max);
        }
        if (_max && num_value > *_max) {
            throw range_constraint_exception(param_name, value, _min, _max);
        }
    } catch (const std::invalid_argument&) {
        // Not a valid number - let type conversion handle this
    } catch (const std::out_of_range&) {
        // Out of int64_t range - let type conversion handle this
    }
}

std::unique_ptr<parameter_constraint> range_constraint::clone() const {
    return std::make_unique<range_constraint>(_min, _max);
}

// length_constraint implementation

void length_constraint::validate(const sstring& param_name, const sstring& value) const {
    size_t length = value.size();

    if (_min_length && length < *_min_length) {
        throw length_constraint_exception(param_name, value, _min_length, _max_length);
    }
    if (_max_length && length > *_max_length) {
        throw length_constraint_exception(param_name, value, _min_length, _max_length);
    }
}

std::unique_ptr<parameter_constraint> length_constraint::clone() const {
    return std::make_unique<length_constraint>(_min_length, _max_length);
}

// enum_constraint implementation

void enum_constraint::validate(const sstring& param_name, const sstring& value) const {
    if (std::ranges::find(_allowed_values, value) == _allowed_values.end()) {
        throw enum_constraint_exception(param_name, value, _allowed_values);
    }
}

std::unique_ptr<parameter_constraint> enum_constraint::clone() const {
    return std::make_unique<enum_constraint>(_allowed_values);
}

// parameter_metadata implementation

parameter_metadata::parameter_metadata(const parameter_metadata& other)
    : _name(other._name)
    , _type(other._type)
    , _location(other._location)
    , _required(other._required)
    , _default_value(other._default_value) {
    // Deep copy constraints
    _constraints.reserve(other._constraints.size());
    for (const auto& constraint : other._constraints) {
        _constraints.push_back(constraint->clone());
    }
}

parameter_metadata& parameter_metadata::operator=(const parameter_metadata& other) {
    if (this != &other) {
        _name = other._name;
        _type = other._type;
        _location = other._location;
        _required = other._required;
        _default_value = other._default_value;

        // Deep copy constraints
        _constraints.clear();
        _constraints.reserve(other._constraints.size());
        for (const auto& constraint : other._constraints) {
            _constraints.push_back(constraint->clone());
        }
    }
    return *this;
}

parameter_metadata& parameter_metadata::with_range(std::optional<int64_t> min,
                                                   std::optional<int64_t> max) {
    _constraints.push_back(std::make_unique<range_constraint>(min, max));
    return *this;
}

parameter_metadata& parameter_metadata::with_length(std::optional<size_t> min_length,
                                                    std::optional<size_t> max_length) {
    _constraints.push_back(std::make_unique<length_constraint>(min_length, max_length));
    return *this;
}

parameter_metadata& parameter_metadata::with_enum(std::vector<sstring> allowed_values) {
    _constraints.push_back(std::make_unique<enum_constraint>(std::move(allowed_values)));
    return *this;
}

parameter_metadata& parameter_metadata::with_default(sstring default_value) {
    _default_value = std::move(default_value);
    return *this;
}

void parameter_metadata::validate(const sstring& value) const {
    for (const auto& constraint : _constraints) {
        constraint->validate(_name, value);
    }
}

// parameter_metadata_registry implementation

void parameter_metadata_registry::register_parameter(parameter_metadata metadata) {
    _params.emplace(metadata.name(), std::move(metadata));
}

const parameter_metadata* parameter_metadata_registry::get_metadata(const sstring& name) const {
    auto it = _params.find(name);
    if (it != _params.end()) {
        return &it->second;
    }
    return nullptr;
}

bool parameter_metadata_registry::has_metadata(const sstring& name) const {
    return _params.find(name) != _params.end();
}



} // namespace seastar::httpd
