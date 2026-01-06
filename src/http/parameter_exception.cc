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

#include <seastar/http/parameter_exception.hh>
#include <seastar/core/print.hh>
#include <fmt/format.h>
#include <fmt/ranges.h>

namespace seastar::httpd {


std::string parameter_validation_exception::format_message(
        const sstring& param_name,
        const sstring& param_value,
        const sstring& reason) {
    if (!param_value.empty()) {
        return fmt::format("Parameter '{}' validation failed (value: '{}'): {}",
                          param_name, param_value, reason);
    } else {
        return fmt::format("Parameter '{}' validation failed: {}",
                          param_name, reason);
    }
}

type_conversion_exception::type_conversion_exception(
        const sstring& param_name,
        const sstring& param_value,
        const sstring& target_type)
    : parameter_validation_exception(
        param_name,
        param_value,
        format("Cannot convert to type '{}'", target_type))
    , _target_type(target_type) {
}

constraint_violation_exception::constraint_violation_exception(
        const sstring& param_name,
        const sstring& param_value,
        const sstring& constraint_description)
    : parameter_validation_exception(param_name, param_value, constraint_description)
    , _constraint_description(constraint_description) {
}

range_constraint_exception::range_constraint_exception(
        const sstring& param_name,
        const sstring& param_value,
        std::optional<int64_t> min,
        std::optional<int64_t> max)
    : constraint_violation_exception(
        param_name,
        param_value,
        [&]() -> std::string {
            if (min && max) {
                return fmt::format("Value must be between {} and {}", *min, *max);
            } else if (min) {
                return fmt::format("Value must be at least {}", *min);
            } else if (max) {
                return fmt::format("Value must be at most {}", *max);
            }
            return "Value constraint violated";
        }())
    , _min(min)
    , _max(max) {
}

length_constraint_exception::length_constraint_exception(
        const sstring& param_name,
        const sstring& param_value,
        std::optional<size_t> min_length,
        std::optional<size_t> max_length)
    : constraint_violation_exception(
        param_name,
        param_value,
        [&]() -> std::string {
            if (min_length && max_length) {
                return fmt::format("Length must be between {} and {}", *min_length, *max_length);
            } else if (min_length) {
                return fmt::format("Length must be at least {}", *min_length);
            } else if (max_length) {
                return fmt::format("Length must be at most {}", *max_length);
            }
            return "Length constraint violated";
        }())
    , _min_length(min_length)
    , _max_length(max_length) {
}

enum_constraint_exception::enum_constraint_exception(
        const sstring& param_name,
        const sstring& param_value,
        const std::vector<sstring>& allowed_values)
    : constraint_violation_exception(
        param_name,
        param_value,
        fmt::format("Value must be one of: {}",
                   fmt::join(allowed_values, ", ")))
    , _allowed_values(allowed_values) {
}



} // namespace seastar::httpd
