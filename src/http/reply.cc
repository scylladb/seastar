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
#endif

namespace seastar {

namespace http {

namespace status_strings {

static const std::unordered_map<reply::status_type, std::string_view> status_strings = {
    {reply::status_type::continue_, "100 Continue"},
    {reply::status_type::switching_protocols, "101 Switching Protocols"},
    {reply::status_type::ok, "200 OK"},
    {reply::status_type::created, "201 Created"},
    {reply::status_type::accepted, "202 Accepted"},
    {reply::status_type::nonauthoritative_information, "203 Non-Authoritative Information"},
    {reply::status_type::no_content, "204 No Content"},
    {reply::status_type::reset_content, "205 Reset Content"},
    {reply::status_type::partial_content, "206 Partial Content"},
    {reply::status_type::multiple_choices, "300 Multiple Choices"},
    {reply::status_type::moved_permanently, "301 Moved Permanently"},
    {reply::status_type::moved_temporarily, "302 Moved Temporarily"},
    {reply::status_type::see_other, "303 See Other"},
    {reply::status_type::not_modified, "304 Not Modified"},
    {reply::status_type::use_proxy, "305 Use Proxy"},
    {reply::status_type::temporary_redirect, "307 Temporary Redirect"},
    {reply::status_type::bad_request, "400 Bad Request"},
    {reply::status_type::unauthorized, "401 Unauthorized"},
    {reply::status_type::payment_required, "402 Payment Required"},
    {reply::status_type::forbidden, "403 Forbidden"},
    {reply::status_type::not_found, "404 Not Found"},
    {reply::status_type::method_not_allowed, "405 Method Not Allowed"},
    {reply::status_type::not_acceptable, "406 Not Acceptable"},
    {reply::status_type::request_timeout, "408 Request Timeout"},
    {reply::status_type::conflict, "409 Conflict"},
    {reply::status_type::gone, "410 Gone"},
    {reply::status_type::length_required, "411 Length Required"},
    {reply::status_type::payload_too_large, "413 Payload Too Large"},
    {reply::status_type::uri_too_long, "414 URI Too Long"},
    {reply::status_type::unsupported_media_type, "415 Unsupported Media Type"},
    {reply::status_type::expectation_failed, "417 Expectation Failed"},
    {reply::status_type::page_expired, "419 Page Expired"},
    {reply::status_type::unprocessable_entity, "422 Unprocessable Entity"},
    {reply::status_type::upgrade_required, "426 Upgrade Required"},
    {reply::status_type::too_many_requests, "429 Too Many Requests"},
    {reply::status_type::login_timeout, "440 Login Timeout"},
    {reply::status_type::internal_server_error, "500 Internal Server Error"},
    {reply::status_type::not_implemented, "501 Not Implemented"},
    {reply::status_type::bad_gateway, "502 Bad Gateway"},
    {reply::status_type::service_unavailable, "503 Service Unavailable"},
    {reply::status_type::gateway_timeout, "504 Gateway Timeout"},
    {reply::status_type::http_version_not_supported, "505 HTTP Version Not Supported"},
    {reply::status_type::insufficient_storage, "507 Insufficient Storage"},
    {reply::status_type::bandwidth_limit_exceeded, "509 Bandwidth Limit Exceeded"},
    {reply::status_type::network_read_timeout, "598 Network Read Timeout"},
    {reply::status_type::network_connect_timeout, "599 Network Connect Timeout"}};

static const std::string_view& to_string_view(reply::status_type status) {
  if (auto found = status_strings.find(status); found != status_strings.end()) [[likely]]
    return found->second;
  return status_strings.at(reply::status_type::internal_server_error);
}

} // namespace status_strings

std::ostream& operator<<(std::ostream& os, reply::status_type st) {
    return os << status_strings::to_string_view(st);
}

sstring reply::response_line() const {
    return seastar::format("HTTP/{} {}\r\n", _version, status_strings::to_string_view(_status));
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
        return _body_writer(http::internal::make_http_chunked_output_stream(con.out()));
    });

}

future<> reply::write_reply_headers(httpd::connection& con) {
    return do_for_each(_headers, [&con](auto& h) {
        return con.out().write(h.first + ": " + h.second + "\r\n");
    });
}

}
} // namespace server
