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
#include <seastar/http/reply.hh>
#include <seastar/core/print.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/common.hh>
#include <seastar/http/response_parser.hh>
#include <seastar/core/loop.hh>

namespace seastar {

namespace http {

namespace status_strings {

const sstring continue_ = "100 Continue";
const sstring switching_protocols = "101 Switching Protocols";
const sstring ok = "200 OK";
const sstring created = "201 Created";
const sstring accepted = "202 Accepted";
const sstring nonauthoritative_information = "203 Non-Authoritative Information";
const sstring no_content = "204 No Content";
const sstring reset_content = "205 Reset Content";
const sstring partial_content = "206 Partial Content";
const sstring multiple_choices = "300 Multiple Choices";
const sstring moved_permanently = "301 Moved Permanently";
const sstring moved_temporarily = "302 Moved Temporarily";
const sstring see_other = "303 See Other";
const sstring not_modified = "304 Not Modified";
const sstring use_proxy = "305 Use Proxy";
const sstring temporary_redirect = "307 Temporary Redirect";
const sstring bad_request = "400 Bad Request";
const sstring unauthorized = "401 Unauthorized";
const sstring payment_required = "402 Payment Required";
const sstring forbidden = "403 Forbidden";
const sstring not_found = "404 Not Found";
const sstring method_not_allowed = "405 Method Not Allowed";
const sstring not_acceptable = "406 Not Acceptable";
const sstring request_timeout = "408 Request Timeout";
const sstring conflict = "409 Conflict";
const sstring gone = "410 Gone";
const sstring length_required = "411 Length Required";
const sstring payload_too_large = "413 Payload Too Large";
const sstring uri_too_long = "414 URI Too Long";
const sstring unsupported_media_type = "415 Unsupported Media Type";
const sstring expectation_failed = "417 Expectation Failed";
const sstring unprocessable_entity = "422 Unprocessable Entity";
const sstring upgrade_required = "426 Upgrade Required";
const sstring too_many_requests = "429 Too Many Requests";
const sstring internal_server_error = "500 Internal Server Error";
const sstring not_implemented = "501 Not Implemented";
const sstring bad_gateway = "502 Bad Gateway";
const sstring service_unavailable = "503 Service Unavailable";
const sstring gateway_timeout = "504 Gateway Timeout";
const sstring http_version_not_supported = "505 HTTP Version Not Supported";
const sstring insufficient_storage = "507 Insufficient Storage";

static const sstring& to_string(reply::status_type status) {
    switch (status) {
    case reply::status_type::continue_:
        return continue_;
    case reply::status_type::switching_protocols:
        return switching_protocols;
    case reply::status_type::ok:
        return ok;
    case reply::status_type::created:
        return created;
    case reply::status_type::accepted:
        return accepted;
    case reply::status_type::nonauthoritative_information:
        return nonauthoritative_information;
    case reply::status_type::no_content:
        return no_content;
    case reply::status_type::reset_content:
        return reset_content;
    case reply::status_type::partial_content:
        return partial_content;
    case reply::status_type::multiple_choices:
        return multiple_choices;
    case reply::status_type::moved_permanently:
        return moved_permanently;
    case reply::status_type::moved_temporarily:
        return moved_temporarily;
    case reply::status_type::see_other:
        return see_other;
    case reply::status_type::not_modified:
        return not_modified;
    case reply::status_type::use_proxy:
        return use_proxy;
    case reply::status_type::temporary_redirect:
        return temporary_redirect;
    case reply::status_type::bad_request:
        return bad_request;
    case reply::status_type::unauthorized:
        return unauthorized;
    case reply::status_type::payment_required:
        return payment_required;
    case reply::status_type::forbidden:
        return forbidden;
    case reply::status_type::not_found:
        return not_found;
    case reply::status_type::method_not_allowed:
        return method_not_allowed;
    case reply::status_type::not_acceptable:
        return not_acceptable;
    case reply::status_type::request_timeout:
        return request_timeout;
    case reply::status_type::conflict:
        return conflict;
    case reply::status_type::gone:
        return gone;
    case reply::status_type::length_required:
        return length_required;
    case reply::status_type::payload_too_large:
        return payload_too_large;
    case reply::status_type::uri_too_long:
        return uri_too_long;
    case reply::status_type::unsupported_media_type:
        return unsupported_media_type;
    case reply::status_type::expectation_failed:
        return expectation_failed;
    case reply::status_type::unprocessable_entity:
        return unprocessable_entity;
    case reply::status_type::upgrade_required:
        return upgrade_required;
    case reply::status_type::too_many_requests:
        return too_many_requests;
    case reply::status_type::internal_server_error:
        return internal_server_error;
    case reply::status_type::not_implemented:
        return not_implemented;
    case reply::status_type::bad_gateway:
        return bad_gateway;
    case reply::status_type::service_unavailable:
        return service_unavailable;
    case reply::status_type::gateway_timeout:
        return gateway_timeout;
    case reply::status_type::http_version_not_supported:
        return http_version_not_supported;
    case reply::status_type::insufficient_storage:
        return insufficient_storage;
    default:
        return internal_server_error;
    }
}
} // namespace status_strings

std::ostream& operator<<(std::ostream& os, reply::status_type st) {
    return os << status_strings::to_string(st);
}

reply::reply(http_response&& resp)
        : _status(static_cast<status_type>(resp._status_code))
        , _headers(std::move(resp._headers))
        , _version(std::move(resp._version))
{
    sstring length_header = get_header("Content-Length");
    content_length = strtol(length_header.c_str(), nullptr, 10);
}

sstring reply::response_line() {
    return "HTTP/" + _version + " " + status_strings::to_string(_status) + "\r\n";
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
