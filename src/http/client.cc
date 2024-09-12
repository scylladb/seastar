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

#include <concepts>
#include <coroutine>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/loop.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/reactor.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/net/tls.hh>
#include <seastar/http/client.hh>
#include <seastar/http/request.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/response_parser.hh>
#include <seastar/http/internal/content_source.hh>
#include <seastar/util/short_streams.hh>
#include <seastar/util/string_utils.hh>
#endif

namespace seastar {
logger http_log("http");
namespace http {
namespace internal {

client_ref::client_ref(http::experimental::client* c) noexcept : _c(c) {
    _c->_nr_connections++;
}

client_ref::~client_ref() {
    if (_c != nullptr) {
        _c->_nr_connections--;
        _c->_wait_con.broadcast();
    }
}

}

namespace experimental {

connection::connection(connected_socket&& fd, internal::client_ref cr)
        : _fd(std::move(fd))
        , _read_buf(_fd.input())
        , _write_buf(_fd.output())
        , _closed(_fd.wait_input_shutdown().finally([me = shared_from_this()] {}))
        , _ref(std::move(cr))
{
}

future<> connection::write_body(const request& req) {
    if (req.body_writer) {
        if (req.content_length != 0) {
            co_await req.body_writer(internal::make_http_content_length_output_stream(_write_buf, req.content_length, req._bytes_written));
            if (req.content_length != req._bytes_written) {
                throw std::runtime_error(format("partial request body write, need {} sent {}", req.content_length, req._bytes_written));
            }
        } else {
            co_await req.body_writer(internal::make_http_chunked_output_stream(_write_buf));
            co_await _write_buf.write("0\r\n\r\n");
        }
    } else if (!req.content.empty()) {
        co_await _write_buf.write(req.content);
    }
}

void connection::setup_request(request& req) {
    if (req._version.empty()) {
        req._version = "1.1";
    }
    if (req.content_length != 0) {
        if (!req.body_writer && req.content.empty()) {
            throw std::runtime_error("Request body writer not set and content is empty");
        }
        req._headers["Content-Length"] = to_sstring(req.content_length);
    }
}

future<connection::reply_ptr> connection::recv_reply() {
    http_response_parser parser;
    parser.init();
    co_await _read_buf.consume(parser);
    if (parser.eof()) {
        http_log.trace("Parsing response EOFed");
        throw std::system_error(ECONNABORTED, std::system_category());
    }
    if (parser.failed()) {
        http_log.trace("Parsing response failed");
        throw std::runtime_error("Invalid http server response");
    }

    auto resp = parser.get_parsed_response();
    sstring length_header = resp->get_header("Content-Length");
    resp->content_length = strtol(length_header.c_str(), nullptr, 10);
    if ((resp->_version != "1.1") || seastar::internal::case_insensitive_cmp()(resp->get_header("Connection"), "close")) {
        _persistent = false;
    }
    co_return resp;
}

future<connection::reply_ptr> connection::do_make_request(request& req) {
    setup_request(req);
    co_await _write_buf.write(req.request_line());
    co_await req.write_request_headers(_write_buf);
    co_await _write_buf.write("\r\n", 2);

    if (req.get_header("Expect") != "") {
        co_await _write_buf.flush();
        reply_ptr rep = co_await recv_reply();
        if (rep->_status != reply::status_type::continue_) {
            co_return rep;
        }
    }

    co_await write_body(req);
    co_await _write_buf.flush();
    co_return co_await recv_reply();
}

future<reply> connection::make_request(request req) {
    reply_ptr rep = co_await do_make_request(req);
    co_return std::move(*rep);
}

input_stream<char> connection::in(reply& rep) {
    if (seastar::internal::case_insensitive_cmp()(rep.get_header("Transfer-Encoding"), "chunked")) {
        return input_stream<char>(data_source(std::make_unique<httpd::internal::chunked_source_impl>(_read_buf, rep.chunk_extensions, rep.trailing_headers)));
    }

    return input_stream<char>(data_source(std::make_unique<httpd::internal::content_length_source_impl>(_read_buf, rep.content_length)));
}

void connection::shutdown() noexcept {
    _persistent = false;
    _fd.shutdown_input();
}

future<> connection::close() {
    co_await when_all(_read_buf.close(), _write_buf.close());
    auto la = _fd.local_address();
    co_await std::move(_closed);
    http_log.trace("destroyed connection {}", la);
}

class basic_connection_factory : public connection_factory {
    socket_address _addr;
public:
    explicit basic_connection_factory(socket_address addr)
            : _addr(std::move(addr))
    {
    }
    virtual future<connected_socket> make(abort_source* as) override {
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
    virtual future<connected_socket> make(abort_source* as) override {
        return tls::connect(_creds, _addr, tls::tls_options{.server_name = _host});
    }
};

client::client(socket_address addr, shared_ptr<tls::certificate_credentials> creds, sstring host)
        : client(std::make_unique<tls_connection_factory>(std::move(addr), std::move(creds), std::move(host)))
{
}

client::client(std::unique_ptr<connection_factory> f, unsigned max_connections, retry_requests retry)
        : _new_connections(std::move(f))
        , _max_connections(max_connections)
        , _retry(retry)
{
}

future<client::connection_ptr> client::get_connection(abort_source* as) {
try_again:
    if (!_pool.empty()) {
        connection_ptr con = _pool.front().shared_from_this();
        _pool.pop_front();
        http_log.trace("pop http connection {} from pool", con->_fd.local_address());
        co_return con;
    }

    if (_nr_connections >= _max_connections) {
        auto sub = as ? as->subscribe([this] () noexcept { _wait_con.broadcast(); }) : std::nullopt;
        co_await _wait_con.wait();
        if (as != nullptr && as->abort_requested()) {
            std::rethrow_exception(as->abort_requested_exception_ptr());
        }
        goto try_again;
    }

    co_return co_await make_connection(as);
}

future<client::connection_ptr> client::make_connection(abort_source* as) {
    _total_new_connections++;
    auto cr = internal::client_ref(this);
    connected_socket cs = co_await _new_connections->make(as);
    http_log.trace("created new http connection {}", cs.local_address());
    auto con = seastar::make_shared<connection>(std::move(cs), std::move(cr));
    co_return con;
}

future<> client::put_connection(connection_ptr con) {
    if (con->_persistent && (_nr_connections <= _max_connections)) {
        http_log.trace("push http connection {} to pool", con->_fd.local_address());
        _pool.push_back(*con);
        _wait_con.signal();
        co_return;
    }

    http_log.trace("dropping connection {}", con->_fd.local_address());
    co_await con->close();
}

future<> client::shrink_connections() {
    while (_nr_connections > _max_connections) {
        if (!_pool.empty()) {
            connection_ptr con = _pool.front().shared_from_this();
            _pool.pop_front();
            co_await con->close();
            continue;
        }

        co_await _wait_con.wait();
    }
}

future<> client::set_maximum_connections(unsigned nr) {
    if (nr > _max_connections) {
        _max_connections = nr;
        _wait_con.broadcast();
        co_return;
    }

    _max_connections = nr;
    co_await shrink_connections();
}

template <std::invocable<connection&> Fn>
futurize_t<std::invoke_result_t<Fn, connection&>> client::with_connection(Fn fn, abort_source* as) {
    connection_ptr con = co_await get_connection(as);
    auto f = co_await coroutine::as_future(futurize_invoke(std::move(fn), *con));
    co_await put_connection(std::move(con));
    co_return co_await std::move(f);
}

template <typename Fn>
requires std::invocable<Fn, connection&>
futurize_t<std::invoke_result_t<Fn, connection&>> client::with_new_connection(Fn fn, abort_source* as) {
    connection_ptr con = co_await make_connection(as);
    auto f = co_await coroutine::as_future(futurize_invoke(std::move(fn), *con));
    co_await put_connection(std::move(con));
    co_return co_await std::move(f);
}

future<> client::make_request(request req, reply_handler handle, std::optional<reply::status_type> expected) {
    return do_make_request(std::move(req), std::move(handle), nullptr, std::move(expected));
}

future<> client::make_request(request req, reply_handler handle, abort_source& as, std::optional<reply::status_type> expected) {
    return do_make_request(std::move(req), std::move(handle), &as, std::move(expected));
}

future<> client::do_make_request(request req, reply_handler handle, abort_source* as, std::optional<reply::status_type> expected) {
    try {
        co_return co_await with_connection(coroutine::lambda([this, &req, &handle, as, expected] (connection& con) -> future<> {
            co_await do_make_request(con, req, handle, as, expected);
        }), as);
    } catch (const std::system_error& ex) {
        if (as && as->abort_requested()) {
            std::rethrow_exception(as->abort_requested_exception_ptr());
        }

        if (!_retry) {
            throw;
        }

        auto code = ex.code().value();
        if ((code != EPIPE) && (code != ECONNABORTED)) {
            throw;
        }
    }

    // The 'con' connection may not yet be freed, so the total connection
    // count still account for it and with_new_connection() may temporarily
    // break the limit. That's OK, the 'con' will be closed really soon
    co_await with_new_connection(coroutine::lambda([this, &req, &handle, as, expected] (connection& con) -> future<> {
        co_await do_make_request(con, req, handle, as, expected);
    }), as);
}

future<> client::do_make_request(connection& con, request& req, reply_handler& handle, abort_source* as, std::optional<reply::status_type> expected) {
    auto sub = as ? as->subscribe([&con] () noexcept { con.shutdown(); }) : std::nullopt;
    try {
        connection::reply_ptr reply = co_await con.do_make_request(req);
        auto& rep = *reply;
        if (expected.has_value() && rep._status != expected.value()) {
            if (http_log.is_enabled(log_level::debug)) {
                auto in = con.in(*reply);
                auto message = co_await util::read_entire_stream_contiguous(in);
                http_log.debug("request finished with {}: {}", reply->_status, message);
            }
            throw httpd::unexpected_status_error(reply->_status);
        }

        co_await handle(rep, con.in(rep));
    } catch (...) {
        con._persistent = false;
        throw;
    }
}

future<> client::close() {
    while (!_pool.empty()) {
        connection_ptr con = _pool.front().shared_from_this();
        _pool.pop_front();
        http_log.trace("closing connection {}", con->_fd.local_address());
        co_await con->close();
    }
}

} // experimental namespace
} // http namespace
} // seastar namespace
