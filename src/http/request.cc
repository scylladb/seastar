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
 * Copyright (C) 2022 Scylladb, Ltd.
 */

#ifdef SEASTAR_MODULE
module;
#endif

#include <string_view>
#include <unordered_map>
#include <utility>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/http/request.hh>
#include <seastar/http/url.hh>
#include <seastar/http/common.hh>
#endif
#include <seastar/util/assert.hh>

namespace seastar {
namespace http {

namespace internal {
// NOTE: Remove this once `query_parameters` is removed
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    const std::unordered_map<sstring, sstring>& deprecated_query_parameters(const request& r) noexcept {
        return r.query_parameters;
    }
    std::unordered_map<sstring, sstring>& deprecated_query_parameters(request& r) noexcept {
        return r.query_parameters;
    }
#pragma GCC diagnostic pop
}

sstring request::format_url() const {
    sstring query = "";
    if (!_query_params.empty()) {
        sstring delim = "&";
        for (const auto& [key, values] : _query_params) {
            auto key_component = delim + internal::url_encode(key) + "=";
            if (values.empty()) {
                query += key_component;
            } else {
                for (const auto& val : values) {
                    query += key_component + internal::url_encode(val);
                }
            }
        }
        if (!query.empty()) {
            query[0] = '?';
        }
    } else {
        sstring delim = "?";
        for (const auto& p : internal::deprecated_query_parameters(*this)) {
            query += delim + internal::url_encode(p.first);
            if (!p.second.empty()) {
                query += "=" + internal::url_encode(p.second);
            }
            delim = "&";
        }
    }

    return _url + query;
}

sstring request::request_line() const {
    SEASTAR_ASSERT(!_version.empty());
    return _method + " " + format_url() + " HTTP/" + _version + "\r\n";
}

// FIXME -- generalize with reply::write_request_headers
future<> request::write_request_headers(output_stream<char>& out) const {
    return do_for_each(_headers, [&out] (auto& h) {
        return out.write(h.first + ": " + h.second + "\r\n");
    });
}

void request::add_query_param(std::string_view param) {
    size_t split = param.find('=');
    auto& deprecated_query_parameters = http::internal::deprecated_query_parameters(*this);
    if (split >= param.length() - 1) {
        sstring key;
        if (http::internal::url_decode(param.substr(0,split) , key)) {
            _query_params[key].push_back("");
            deprecated_query_parameters[key] = "";
        }
    } else {
        sstring key;
        sstring value;
        if (http::internal::url_decode(param.substr(0,split), key)
                && http::internal::url_decode(param.substr(split + 1), value)) {
            deprecated_query_parameters[key] = value;
            _query_params[key].push_back(std::move(value));
        }
    }

}

sstring request::parse_query_param() {
    http::internal::deprecated_query_parameters(*this).clear();
    _query_params.clear();
    size_t pos = _url.find('?');
    if (pos == sstring::npos) {
        return _url;
    }
    size_t curr = pos + 1;
    size_t end_param;
    std::string_view url = _url;
    while ((end_param = _url.find('&', curr)) != sstring::npos) {
        add_query_param(url.substr(curr, end_param - curr) );
        curr = end_param + 1;
    }
    add_query_param(url.substr(curr));
    return _url.substr(0, pos);
}

void request::write_body(const sstring& content_type, sstring content) {
    set_content_type(content_type);
    content_length = content.size();
    _headers["Content-Length"] = to_sstring(content_length);
    internal::deprecated_content(*this) = std::move(content);
}

void request::write_body(const sstring& content_type, body_writer_type&& body_writer) {
    set_content_type(content_type);
    _headers["Transfer-Encoding"] = "chunked";
    this->body_writer = std::move(body_writer);
}

void request::write_body(const sstring& content_type, size_t len, body_writer_type&& body_writer) {
    set_content_type(content_type);
    content_length = len;
    _headers["Content-Length"] = to_sstring(content_length);
    if (len > 0) {
        // At the time of this writing, connection::write_body()
        // assumes that `body_writer` is unset if `content_length` is 0.
        this->body_writer = std::move(body_writer);
    }
}

void request::set_expects_continue() {
    _headers["Expect"] = "100-continue";
}

request request::make(sstring method, sstring host, sstring path) {
    request rq;
    rq._version = "1.1";
    rq._method = std::move(method);
    rq._url = std::move(path);
    rq._headers["Host"] = std::move(host);
    return rq;
}

request request::make(httpd::operation_type type, sstring host, sstring path) {
    return make(httpd::type2str(type), std::move(host), std::move(path));
}

} // http namespace
} // seastar namespace
