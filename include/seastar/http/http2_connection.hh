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
 * Copyright (C) 2025 Scylladb, Ltd.
 */

#pragma once

#include <seastar/core/iostream.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/pipe.hh>
#include <seastar/net/api.hh>
#include <seastar/http/request.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/routes.hh>
#include <seastar/http/http2_frame_parser.hh>

namespace seastar {

namespace http {

class http2_connection {
public:
    // Represents a single HTTP/2 stream (a request/response exchange)
    // As per RFC 7540
    struct h2_stream {
        // A stream has a well-defined lifecycle (RFC 7540, Section 5.1).
        enum class state {
            IDLE,
            RESERVED_LOCAL,
            RESERVED_REMOTE,
            OPEN,
            HALF_CLOSED_REMOTE,
            HALF_CLOSED_LOCAL,
            CLOSED
        };
    };

private:
    httpd::routes& _routes;
    connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    http2_frame_header_parser _parser;
    bool _done = false;

public:
    http2_connection(httpd::routes& routes, connected_socket&& fd, socket_address remote_addr, socket_address local_addr)
        : _routes(routes)
        , _fd(std::move(fd))
        , _read_buf(_fd.input())
        , _write_buf(_fd.output())
    {}

    // Main entry point to start processing the HTTP/2 connection.
    future<> process();

    #ifdef SEASTAR_HTTP2_TEST
        http2_frame_header_parser& get_parser_for_testing() { return _parser; }
    #endif

private:
    // The main loop for reading and dispatching frames.
    future<> read_loop();
    future<> read_one_frame();

    // Frame-specific handlers.
    future<> handle_settings_frame(const temporary_buffer<char>& payload);
    future<> handle_headers_frame(uint32_t stream_id, uint8_t flags, const temporary_buffer<char>& payload);
    future<> handle_data_frame(uint32_t stream_id, uint8_t flags, const temporary_buffer<char>& payload);
    // ... other frame handlers (GOAWAY, RST_STREAM, etc.)
};



} // namespace http

} // namespace seastar 