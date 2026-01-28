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

/// Implementation file for request.hh template methods
/// This file provides the implementations for typed parameter access
/// Include this after request.hh when you need to use typed parameter methods

#include <seastar/http/request.hh>
#include <seastar/http/parameter_metadata.hh>
#include <seastar/http/parameter_converter.hh>
#include <seastar/http/exception.hh>

namespace seastar::http {

template<typename T>
inline T request::get(std::string_view key, const T& default_value) const {
    // Get raw string value (query or path parameter)
    sstring raw_value;
    bool found = false;

    // Try query parameter first
    if (has_query_param(key)) {
        raw_value = get_query_param(key);
        found = true;
    } else if (param.exists(sstring(key))) {
        // Try path parameter
        raw_value = get_path_param(sstring(key));
        found = true;
    }

    if (!found) {
        return default_value;
    }

    // Validate if metadata exists
    if (_param_metadata_registry) {
        auto* metadata = _param_metadata_registry->get_metadata(sstring(key));
        if (metadata) {
            metadata->validate(raw_value);
        }
    }

    return httpd::parameter_converter<T>::convert(raw_value);
}

template<typename T>
inline T request::get(std::string_view key) const {
    sstring raw_value;
    bool found = false;

    if (has_query_param(key)) {
        raw_value = get_query_param(key);
        found = true;
    } else if (param.exists(sstring(key))) {
        raw_value = get_path_param(sstring(key));
        found = true;
    }

    if (!found) {
        throw httpd::missing_param_exception(sstring(key));
    }

    // Validate if metadata exists
    if (_param_metadata_registry) {
        auto* metadata = _param_metadata_registry->get_metadata(sstring(key));
        if (metadata) {
            metadata->validate(raw_value);
        }
    }

    return httpd::parameter_converter<T>::convert(raw_value);
}

} // namespace seastar::http
