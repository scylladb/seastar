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

//
// response.cpp
// ~~~~~~~~~
//
// Copyright (c) 2003-2013 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifdef SEASTAR_MODULE
module;
#include <iostream>
#include <utility>
#include <unordered_map>
module seastar;
#else
#include <seastar/http/reply.hh>
#include <seastar/core/print.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/common.hh>
#include <seastar/http/response_parser.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/reactor.hh>
#endif

template<>
struct std::hash<seastar::http::status_type> : public std::hash<int>
{};

namespace seastar {

namespace http {

namespace status_strings {

static auto& status_strings() {
    static std::unordered_map<status_type, std::string_view> status_strings = {
        {reply::status_type::continue_, "Continue"},
        {reply::status_type::switching_protocols, "Switching Protocols"},
        {reply::status_type::ok, "OK"},
        {reply::status_type::created, "Created"},
        {reply::status_type::accepted, "Accepted"},
        {reply::status_type::nonauthoritative_information, "Non-Authoritative Information"},
        {reply::status_type::no_content, "No Content"},
        {reply::status_type::reset_content, "Reset Content"},
        {reply::status_type::partial_content, "Partial Content"},
        {reply::status_type::multiple_choices, "Multiple Choices"},
        {reply::status_type::moved_permanently, "Moved Permanently"},
        {reply::status_type::moved_temporarily, "Moved Temporarily"},
        {reply::status_type::see_other, "See Other"},
        {reply::status_type::not_modified, "Not Modified"},
        {reply::status_type::use_proxy, "Use Proxy"},
        {reply::status_type::temporary_redirect, "Temporary Redirect"},
        {reply::status_type::permanent_redirect, "Permanent Redirect"},
        {reply::status_type::bad_request, "Bad Request"},
        {reply::status_type::unauthorized, "Unauthorized"},
        {reply::status_type::payment_required, "Payment Required"},
        {reply::status_type::forbidden, "Forbidden"},
        {reply::status_type::not_found, "Not Found"},
        {reply::status_type::method_not_allowed, "Method Not Allowed"},
        {reply::status_type::not_acceptable, "Not Acceptable"},
        {reply::status_type::request_timeout, "Request Timeout"},
        {reply::status_type::conflict, "Conflict"},
        {reply::status_type::gone, "Gone"},
        {reply::status_type::length_required, "Length Required"},
        {reply::status_type::payload_too_large, "Payload Too Large"},
        {reply::status_type::uri_too_long, "URI Too Long"},
        {reply::status_type::unsupported_media_type, "Unsupported Media Type"},
        {reply::status_type::expectation_failed, "Expectation Failed"},
        {reply::status_type::page_expired, "Page Expired"},
        {reply::status_type::unprocessable_entity, "Unprocessable Entity"},
        {reply::status_type::upgrade_required, "Upgrade Required"},
        {reply::status_type::too_many_requests, "Too Many Requests"},
        {reply::status_type::login_timeout, "Login Timeout"},
        {reply::status_type::internal_server_error, "Internal Server Error"},
        {reply::status_type::not_implemented, "Not Implemented"},
        {reply::status_type::bad_gateway, "Bad Gateway"},
        {reply::status_type::service_unavailable, "Service Unavailable"},
        {reply::status_type::gateway_timeout, "Gateway Timeout"},
        {reply::status_type::http_version_not_supported, "HTTP Version Not Supported"},
        {reply::status_type::insufficient_storage, "Insufficient Storage"},
        {reply::status_type::bandwidth_limit_exceeded, "Bandwidth Limit Exceeded"},
        {reply::status_type::network_read_timeout, "Network Read Timeout"},
        {reply::status_type::network_connect_timeout, "Network Connect Timeout"}};

    return status_strings;
}

template<typename Func>
static auto with_string_view(status_type status, Func&& func) -> std::invoke_result_t<Func, std::string_view> {
    const auto& ss = status_strings();
    if (auto found = ss.find(status); found != ss.end()) [[likely]] {
     return func(found->second);
    }
    auto dummy_buf = std::to_string(int(status));
    return func(dummy_buf);
}

} // namespace status_strings

std::string_view bind_status_name(status_type st, std::string_view name) {
    if (engine_is_ready()) {
        throw std::runtime_error("Cannot bind http status name in runtime");
    }
    auto& map = status_strings::status_strings();
    auto p = map.try_emplace(st, name);
    if (!p.second) {
        throw std::invalid_argument(seastar::format("Status {} {} already bound. Previous name: {}",
            int(st), name, p.first->second
        ));
    }
    return p.first->second;
}

std::ostream& operator<<(std::ostream& os, status_type st) {
    return status_strings::with_string_view(st, [&](std::string_view txt) -> std::ostream& {
        return os << int(st) << " " << txt;
    });
}

std::ostream& operator<<(std::ostream& os, status_type::status_init st) {
    return os << status_type(st);
}

sstring reply::response_line() const {
    return seastar::format("HTTP/{} {}\r\n", _version, _status);
}

void reply::write_body(const sstring& content_type, noncopyable_function<future<>(output_stream<char>&&)>&& body_writer) {
    set_content_type(content_type);
    _body_writer  = std::move(body_writer);
}

void reply::write_body(const sstring& content_type, sstring content) {
    _content = std::move(content);
    done(content_type);
}

future<> reply::write_reply_to_connection(httpd::connection& con) {
    add_header("Transfer-Encoding", "chunked");
    return con.out().write(response_line()).then([this, &con] () mutable {
        return write_reply_headers(con);
    }).then([&con] () mutable {
        return con.out().write("\r\n", 2);
    }).then([this, &con] () mutable {
        if (_skip_body) {
            return make_ready_future<>();
        }
        return _body_writer(http::internal::make_http_chunked_output_stream(con.out())).then([&con] {
            return con.out().write("0\r\n\r\n", 5);
        });
    });

}

future<> reply::write_reply_headers(httpd::connection& con) {
    return do_for_each(_headers, [&con](auto& h) {
        return con.out().write(h.first + ": " + h.second + "\r\n");
    });
}

}
} // namespace server
