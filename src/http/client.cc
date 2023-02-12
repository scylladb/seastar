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
#include <seastar/core/reactor.hh>
#include <seastar/net/tls.hh>
#include <seastar/http/client.hh>
#include <seastar/http/request.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/response_parser.hh>
#include <seastar/http/internal/content_source.hh>

namespace seastar {
logger http_log("http");
namespace http {
namespace experimental {

connection::connection(connected_socket&& fd)
        : _fd(std::move(fd))
        , _read_buf(_fd.input())
        , _write_buf(_fd.output())
        , _closed(_fd.wait_input_shutdown().finally([me = shared_from_this()] {}))
{
}

future<> connection::write_body(request& req) {
    if (req.body_writer) {
        if (req.content_length != 0) {
            auto orig_content_length = req.content_length;
            return req.body_writer(internal::make_http_content_length_output_stream(_write_buf, req.content_length)).then([&req, orig_content_length] {
                if (req.content_length == orig_content_length) {
                    return make_ready_future<>();
                } else {
                    return make_exception_future<>(std::runtime_error(format("partial request body write, need {} sent {}", orig_content_length, req.content_length)));
                }
            });
        }
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

    return input_stream<char>(data_source(std::make_unique<httpd::internal::content_length_source_impl>(_read_buf, rep.content_length)));
}

future<> connection::close() {
    return when_all(_read_buf.close(), _write_buf.close()).discard_result().then([this] {
        auto la = _fd.local_address();
        return std::move(_closed).then([la = std::move(la)] {
            http_log.trace("destroyed connection {}", la);
        });
    });
}

class basic_connection_factory : public connection_factory {
    socket_address _addr;
public:
    explicit basic_connection_factory(socket_address addr)
            : _addr(std::move(addr))
    {
    }
    virtual future<connected_socket> make() override {
        return seastar::connect(_addr, {}, transport::TCP);
    }
};

client::client(socket_address addr)
        : client(std::make_unique<basic_connection_factory>(std::move(addr)))
{
}

class tls_connection_factory : public connection_factory {
    socket_address _addr;
    shared_ptr<tls::certificate_credentials> _creds;
    sstring _host;
public:
    tls_connection_factory(socket_address addr, shared_ptr<tls::certificate_credentials> creds, sstring host)
            : _addr(std::move(addr))
            , _creds(std::move(creds))
            , _host(std::move(host))
    {
    }
    virtual future<connected_socket> make() override {
        return tls::connect(_creds, _addr, _host);
    }
};

client::client(socket_address addr, shared_ptr<tls::certificate_credentials> creds, sstring host)
        : client(std::make_unique<tls_connection_factory>(std::move(addr), std::move(creds), std::move(host)))
{
}

client::client(std::unique_ptr<connection_factory> f)
        : _new_connections(std::move(f))
{
}

future<client::connection_ptr> client::get_connection() {
    if (!_pool.empty()) {
        connection_ptr con = _pool.front().shared_from_this();
        _pool.pop_front();
        http_log.trace("pop http connection {} from pool", con->_fd.local_address());
        return make_ready_future<connection_ptr>(con);
    }

    return _new_connections->make().then([] (connected_socket cs) {
        http_log.trace("created new http connection {}", cs.local_address());
        auto con = seastar::make_shared<connection>(std::move(cs));
        return make_ready_future<connection_ptr>(std::move(con));
    });
}

future<> client::put_connection(connection_ptr con, bool can_cache) {
    if (can_cache) {
        http_log.trace("push http connection {} to pool", con->_fd.local_address());
        _pool.push_back(*con);
        return make_ready_future<>();
    }

    http_log.trace("dropping connection {}", con->_fd.local_address());
    return con->close().finally([con] {});
}

template <typename Fn>
SEASTAR_CONCEPT( requires std::invocable<Fn, connection&> )
auto client::with_connection(Fn&& fn) {
    return get_connection().then([this, fn = std::move(fn)] (connection_ptr con) mutable {
        return fn(*con).then_wrapped([this, con = std::move(con)] (auto f) mutable {
            return put_connection(std::move(con), !f.failed()).then([f = std::move(f)] () mutable {
                return std::move(f);
            });
        });
    });
}

future<> client::make_request(request req, reply_handler handle, reply::status_type expected) {
    return with_connection([req = std::move(req), handle = std::move(handle), expected] (connection& con) mutable {
        return con.make_request(std::move(req)).then([&con, expected, handle = std::move(handle)] (reply rep) mutable {
            if (rep._status != expected) {
                return make_exception_future<>(std::runtime_error(format("request finished with {}", rep._status)));
            }

            return do_with(std::move(rep), [&con, handle = std::move(handle)] (auto& rep) mutable {
                return handle(rep, con.in(rep));
            });
        });
    });
}

future<> client::close() {
    if (_pool.empty()) {
        return make_ready_future<>();
    }

    connection_ptr con = _pool.front().shared_from_this();
    _pool.pop_front();
    http_log.trace("closing connection {}", con->_fd.local_address());
    return con->close().then([this, con] {
        return close();
    });
}

} // experimental namespace
} // http namespace
} // seastar namespace
