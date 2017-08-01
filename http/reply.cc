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
#include "reply.hh"
#include "core/print.hh"
#include "httpd.hh"

namespace seastar {

namespace httpd {

namespace status_strings {

const sstring ok = " 200 OK\r\n";
const sstring created = " 201 Created\r\n";
const sstring accepted = " 202 Accepted\r\n";
const sstring no_content = " 204 No Content\r\n";
const sstring multiple_choices = " 300 Multiple Choices\r\n";
const sstring moved_permanently = " 301 Moved Permanently\r\n";
const sstring moved_temporarily = " 302 Moved Temporarily\r\n";
const sstring not_modified = " 304 Not Modified\r\n";
const sstring bad_request = " 400 Bad Request\r\n";
const sstring unauthorized = " 401 Unauthorized\r\n";
const sstring forbidden = " 403 Forbidden\r\n";
const sstring not_found = " 404 Not Found\r\n";
const sstring internal_server_error = " 500 Internal Server Error\r\n";
const sstring not_implemented = " 501 Not Implemented\r\n";
const sstring bad_gateway = " 502 Bad Gateway\r\n";
const sstring service_unavailable = " 503 Service Unavailable\r\n";

static const sstring& to_string(reply::status_type status) {
    switch (status) {
    case reply::status_type::ok:
        return ok;
    case reply::status_type::created:
        return created;
    case reply::status_type::accepted:
        return accepted;
    case reply::status_type::no_content:
        return no_content;
    case reply::status_type::multiple_choices:
        return multiple_choices;
    case reply::status_type::moved_permanently:
        return moved_permanently;
    case reply::status_type::moved_temporarily:
        return moved_temporarily;
    case reply::status_type::not_modified:
        return not_modified;
    case reply::status_type::bad_request:
        return bad_request;
    case reply::status_type::unauthorized:
        return unauthorized;
    case reply::status_type::forbidden:
        return forbidden;
    case reply::status_type::not_found:
        return not_found;
    case reply::status_type::internal_server_error:
        return internal_server_error;
    case reply::status_type::not_implemented:
        return not_implemented;
    case reply::status_type::bad_gateway:
        return bad_gateway;
    case reply::status_type::service_unavailable:
        return service_unavailable;
    default:
        return internal_server_error;
    }
}
} // namespace status_strings

sstring reply::response_line() {
    return "HTTP/" + _version + status_strings::to_string(_status);
}

class http_chunked_data_sink_impl : public data_sink_impl {
    output_stream<char>& _out;

    future<> write_size(size_t s) {
        auto req = sprint("%x\r\n", s);
        return _out.write(req);
    }
public:
    http_chunked_data_sink_impl(output_stream<char>& out) : _out(out) {
    }
    virtual future<> put(net::packet data)  override { abort(); }
    virtual future<> put(temporary_buffer<char> buf) override {
        if (buf.size() == 0) {
            // size 0 buffer should be ignored, some server
            // may consider it an end of message
            return make_ready_future<>();
        }
        auto size = buf.size();
        return write_size(size).then([this, buf = std::move(buf)] () mutable {
            return _out.write(buf.get(), buf.size());
        }).then([this] () mutable {
            return _out.write("\r\n", 2);
        });
    }
    virtual future<> close() {
        return  make_ready_future<>();
    }
};

class http_chunked_data_sink : public data_sink {
public:
    http_chunked_data_sink(output_stream<char>& out)
        : data_sink(std::make_unique<http_chunked_data_sink_impl>(
                out)) {}
};

static output_stream<char> make_http_chunked_output_stream(output_stream<char>& out) {
    return output_stream<char>(http_chunked_data_sink(out), 32000, true);
}


void reply::write_body(const sstring& content_type, std::function<future<>(output_stream<char>&&)>&& body_writer) {
    set_content_type(content_type);
    _body_writer  = std::move(body_writer);
}

void reply::write_body(const sstring& content_type, const sstring& content) {
    _content = content;
    done(content_type);
}

future<> reply::write_reply_to_connection(connection& con) {
    add_header("Transfer-Encoding", "chunked");
    return con.out().write(response_line()).then([this, &con] () mutable {
        return write_reply_headers(con);
    }).then([&con] () mutable {
        return con.out().write("\r\n", 2);
    }).then([this, &con] () mutable {
        return _body_writer(make_http_chunked_output_stream(con.out()));
    });

}

future<> reply::write_reply_headers(connection& con) {
    return do_for_each(_headers, [&con](auto& h) {
        return con.out().write(h.first + ": " + h.second + "\r\n");
    });
}

}
} // namespace server
