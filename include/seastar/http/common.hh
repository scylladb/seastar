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

#ifndef SEASTAR_MODULE
#include <unordered_map>
#endif

#include <seastar/core/sstring.hh>
#include <seastar/core/iostream.hh>
#include <seastar/http/url.hh>

namespace seastar {

namespace http {
namespace internal {
output_stream<char> make_http_chunked_output_stream(output_stream<char>& out);
// @param total_len defines the maximum number of bytes to be written.
// @param bytes_written after the stream is closed, it is updated with the
//        actual number of bytes written.
output_stream<char> make_http_content_length_output_stream(output_stream<char>& out, size_t total_len, size_t& bytes_written);
} // internal namespace
} // http namespace

namespace httpd {

SEASTAR_MODULE_EXPORT_BEGIN

class parameters {
    // Note: the path matcher adds parameters with the '/' prefix into the params map (eg. "/param1"), and some getters
    // remove this '/' to return just the path parameter value
    std::unordered_map<sstring, sstring> params;
public:
    const sstring& path(const sstring& key) const {
        return params.at(key);
    }

    [[deprecated("Use request::get_path_param() instead.")]]
    sstring operator[](const sstring& key) const {
        return params.at(key).substr(1);
    }

    const sstring& at(const sstring& key) const {
        return path(key);
    }

    sstring get_decoded_param(const sstring& key) const {
        auto res = params.find(key);
        if (res == params.end()) {
            return "";
        }
        auto raw_path_param = res->second.substr(1);
        auto decoded_path_param = sstring{};
        auto ok = seastar::http::internal::path_decode(raw_path_param, decoded_path_param);
        if (!ok) {
            return "";
        }
        return decoded_path_param;
    }

    bool exists(const sstring& key) const {
        return params.find(key) != params.end();
    }

    void set(const sstring& key, const sstring& value) {
        params[key] = value;
    }

    void clear() {
        params.clear();
    }

};

enum operation_type {
    GET, POST, PUT, DELETE, HEAD, OPTIONS, TRACE, CONNECT, PATCH, NUM_OPERATION
};

/**
 * Translate the string command to operation type
 * @param type the string "GET" or "POST"
 * @return the operation_type
 */
operation_type str2type(const sstring& type);

/**
 * Translate the operation type to command string
 * @param type the string GET or POST
 * @return the command string "GET" or "POST"
 */
sstring type2str(operation_type type);

}

SEASTAR_MODULE_EXPORT_END
}
