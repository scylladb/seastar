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
 * Copyright 2015 Cloudius Systems
 */

#pragma once

#include <seastar/http/request.hh>
#include <seastar/http/common.hh>
#include <seastar/http/exception.hh>
#include <seastar/http/reply.hh>


namespace seastar {

namespace httpd {

class parameter_metadata_registry;

typedef const http::request& const_req;

/**
 * handlers holds the logic for serving an incoming request.
 * All handlers inherit from the base handler_base and
 * implement the handle method.
 *
 */
class handler_base {
    std::vector<sstring> _mandatory_param;
    const parameter_metadata_registry* _param_metadata = nullptr;
    bool _skip_validation = false;
protected:
    handler_base() = default;
    handler_base(const handler_base&) = default;
public:
    virtual ~handler_base() = default;
    /**
     * All handlers should implement this method.
     *  It fill the reply according to the request.
     * @param path the url path used in this call
     * @param req the original request
     * @param rep the reply
     */
    virtual future<std::unique_ptr<http::reply> > handle(const sstring& path,
            std::unique_ptr<http::request> req, std::unique_ptr<http::reply> rep) = 0;


    /**
     * Add a mandatory parameter
     * @param param a parameter name
     * @return a reference to the handler
     */
    handler_base& mandatory(const sstring& param) {
        _mandatory_param.push_back(param);
        return *this;
    }

    /**
     * Check if all mandatory parameters exist in the request. if any param
     * does not exist, the function would throw a @c missing_param_exception
     * @param params req the http request
     */
    void verify_mandatory_params(const http::request& req) const {
        for (auto& param : _mandatory_param) {
            if (req.get_query_param(param) == "") {
                throw missing_param_exception(param);
            }
        }
    }

    /**
     * Set parameter metadata for validation
     * Called by the routing layer when metadata is available
     * @param metadata pointer to parameter metadata registry
     */
    void set_parameter_metadata(const parameter_metadata_registry* metadata);

    /**
     * Skip automatic parameter validation for this handler
     * Use this when you need custom validation logic
     * @param skip whether to skip validation (default true)
     * @return a reference to the handler
     */
    handler_base& skip_validation(bool skip = true) {
        _skip_validation = skip;
        return *this;
    }

    /**
     * Validate all parameters according to metadata
     * This is called automatically by the routing layer before handle()
     * Sets up the request with metadata for lazy validation
     * @param req the http request
     */
    void validate_parameters(http::request& req) const;
};

}

}

