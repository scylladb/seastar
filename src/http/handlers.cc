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

#include <seastar/http/handlers.hh>
#include <seastar/http/parameter_metadata.hh>
#include <seastar/http/exception.hh>

namespace seastar::httpd {


void handler_base::set_parameter_metadata(const parameter_metadata_registry* metadata) {
    _param_metadata = metadata;
}

void handler_base::validate_parameters(http::request& req) const {
    if (_skip_validation || !_param_metadata) {
        return;
    }

    // Set metadata on request for lazy validation during get<T>() calls
    req.set_parameter_metadata(_param_metadata);

    // Validate required parameters exist
    for (const auto& [name, metadata] : _param_metadata->all_params()) {
        if (metadata.required()) {
            bool found = req.has_query_param(name) || req.param.exists(name);
            if (!found) {
                throw missing_param_exception(name);
            }
        }
    }
}



} // namespace seastar::httpd
