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

sstring request::format_url() const {
    sstring query = "";
    sstring delim = "?";
    for (const auto& p : query_parameters) {
        query += delim + internal::url_encode(p.first);
        if (!p.second.empty()) {
            query += "=" + internal::url_encode(p.second);
        }
        delim = "&";
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

    if (split >= param.length() - 1) {
        sstring key;
        if (http::internal::url_decode(param.substr(0,split) , key)) {
            query_parameters[key] = "";
        }
    } else {
        sstring key;
        sstring value;
        if (http::internal::url_decode(param.substr(0,split), key)
                && http::internal::url_decode(param.substr(split + 1), value)) {
            query_parameters[key] = std::move(value);
        }
    }

}

sstring request::parse_query_param() {
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
    this->content = std::move(content);
}

void request::write_body(const sstring& content_type, noncopyable_function<future<>(output_stream<char>&&)>&& body_writer) {
    set_content_type(content_type);
    _headers["Transfer-Encoding"] = "chunked";
    this->body_writer = std::move(body_writer);
}

void request::write_body(const sstring& content_type, size_t len, noncopyable_function<future<>(output_stream<char>&&)>&& body_writer) {
    set_content_type(content_type);
    content_length = len;
    this->body_writer = std::move(body_writer);
}

void request::set_expects_continue() {
    _headers["Expect"] = "100-continue";
}

request request::make(sstring method, sstring host, sstring path) {
    request rq;
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
