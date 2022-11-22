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

#include <seastar/core/loop.hh>
#include <seastar/core/when_all.hh>
#include <seastar/http/client.hh>
#include <seastar/http/request.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/response_parser.hh>
#include <seastar/http/internal/content_source.hh>

namespace seastar {
namespace http {
namespace experimental {

connection::connection(connected_socket&& fd)
        : _fd(std::move(fd))
        , _read_buf(_fd.input())
        , _write_buf(_fd.output())
{
}

future<> connection::write_body(request& req) {
    if (req.body_writer) {
        return req.body_writer(internal::make_http_chunked_output_stream(_write_buf)).then([this] {
            return _write_buf.write("0\r\n\r\n");
        });
    } else if (!req.content.empty()) {
        return _write_buf.write(req.content);
    } else {
        return make_ready_future<>();
    }
}

future<std::optional<reply>> connection::maybe_wait_for_continue(request& req) {
    if (req.get_header("Expect") == "") {
        return make_ready_future<std::optional<reply>>(std::nullopt);
    }

    return _write_buf.flush().then([this] {
        return recv_reply().then([] (reply rep) {
            if (rep._status == reply::status_type::continue_) {
                return make_ready_future<std::optional<reply>>(std::nullopt);
            } else {
                return make_ready_future<std::optional<reply>>(std::move(rep));
            }
        });
    });
}

future<> connection::send_request_head(request& req) {
    if (req._version.empty()) {
        req._version = "1.1";
    }
    if (req.content_length != 0) {
        if (!req.body_writer && req.content.empty()) {
            throw std::runtime_error("Request body writer not set and content is empty");
        }
        req._headers["Content-Length"] = to_sstring(req.content_length);
    }

    return _write_buf.write(req.request_line()).then([this, &req] {
        return req.write_request_headers(_write_buf).then([this] {
            return _write_buf.write("\r\n", 2);
        });
    });
}

future<reply> connection::recv_reply() {
    http_response_parser parser;
    return do_with(std::move(parser), [this] (auto& parser) {
        parser.init();
        return _read_buf.consume(parser).then([&parser] {
            if (parser.eof()) {
                throw std::runtime_error("Invalid response");
            }

            auto resp = parser.get_parsed_response();
            return make_ready_future<reply>(std::move(*resp));
        });
    });
}

future<reply> connection::make_request(request req) {
    return do_with(std::move(req), [this] (auto& req) {
        return send_request_head(req).then([this, &req] {
            return maybe_wait_for_continue(req).then([this, &req] (std::optional<reply> cont) {
                if (cont.has_value()) {
                    return make_ready_future<reply>(std::move(*cont));
                }

                return write_body(req).then([this] {
                    return _write_buf.flush().then([this] {
                        return recv_reply();
                    });
                });
            });
        });
    });
}

input_stream<char> connection::in(reply& rep) {
    if (http::request::case_insensitive_cmp()(rep.get_header("Transfer-Encoding"), "chunked")) {
        return input_stream<char>(data_source(std::make_unique<httpd::internal::chunked_source_impl>(_read_buf, rep.chunk_extensions, rep.trailing_headers)));
    }

    sstring length_header = rep.get_header("Content-Length");
    auto content_length = strtol(length_header.c_str(), nullptr, 10);
    return input_stream<char>(data_source(std::make_unique<httpd::internal::content_length_source_impl>(_read_buf, content_length)));
}

future<> connection::close() {
    return when_all(_read_buf.close(), _write_buf.close()).discard_result();
}

} // experimental namespace
} // http namespace
} // seastar namespace
