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
 * Copyright 2021 ScyllaDB
 */

#include <seastar/websocket/server.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/log.hh>
#include <seastar/core/scattered_message.hh>
#include <seastar/http/request.hh>

namespace seastar::experimental::websocket {

static sstring http_upgrade_reply_template =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Accept: ";

void server::listen(socket_address addr, listen_options lo) {
    _listeners.push_back(seastar::listen(addr, lo));
    accept(_listeners.back());
}
void server::listen(socket_address addr) {
    listen_options lo;
    lo.reuse_address = true;
    return listen(addr, lo);
}

void server::accept(server_socket &listener) {
    (void)try_with_gate(_task_gate, [this, &listener]() {
        return repeat([this, &listener]() {
            return accept_one(listener);
        });
    }).handle_exception_type([](const gate_closed_exception &e) {});
}

future<stop_iteration> server::accept_one(server_socket &listener) {
    return listener.accept().then([this](accept_result ar) {
        auto conn = std::make_unique<server_connection>(*this, std::move(ar.connection));
        (void)try_with_gate(_task_gate, [conn = std::move(conn)]() mutable {
            return conn->process().finally([conn = std::move(conn)] {
                websocket_logger.debug("Connection is finished");
            });
        }).handle_exception_type([](const gate_closed_exception &e) {});
        return make_ready_future<stop_iteration>(stop_iteration::no);
    }).handle_exception_type([](const std::system_error &e) {
        // We expect a ECONNABORTED when server::stop is called,
        // no point in warning about that.
        if (e.code().value() != ECONNABORTED) {
            websocket_logger.error("accept failed: {}", e);
        }
        return make_ready_future<stop_iteration>(stop_iteration::yes);
    }).handle_exception([](std::exception_ptr ex) {
        websocket_logger.info("accept failed: {}", ex);
        return make_ready_future<stop_iteration>(stop_iteration::yes);
    });
}

future<> server::stop() {
    for (auto&& l : _listeners) {
        l.abort_accept();
    }

    for (auto&& c : _connections) {
        c.shutdown_input();
    }

    return _task_gate.close().finally([this] {
        return parallel_for_each(_connections, [] (server_connection& conn) {
            return conn.close(true).handle_exception([] (auto ignored) {});
        });
    });
}

server_connection::~server_connection() {
    _server._connections.erase(_server._connections.iterator_to(*this));
}

void server_connection::on_new_connection() {
    _server._connections.push_back(*this);
}

future<> server_connection::process() {
    return when_all_succeed(read_loop(), response_loop()).discard_result().handle_exception([] (const std::exception_ptr& e) {
        websocket_logger.debug("Processing failed: {}", e);
    });
}

future<> server_connection::read_http_upgrade_request() {
    _http_parser.init();
    co_await _read_buf.consume(_http_parser);

    if (_http_parser.eof()) {
        _done = true;
        co_return;
    }
    std::unique_ptr<http::request> req = _http_parser.get_parsed_request();
    if (_http_parser.failed()) {
        throw websocket::exception("Incorrect upgrade request");
    }

    sstring upgrade_header = req->get_header("Upgrade");
    if (upgrade_header != "websocket") {
        throw websocket::exception("Upgrade header missing");
    }

    sstring subprotocol = req->get_header("Sec-WebSocket-Protocol");

    if (!_server.is_handler_registered(subprotocol)) {
        throw websocket::exception("Subprotocol not supported.");
    }
    this->_handler = this->_server._handlers[subprotocol];
    this->_subprotocol = subprotocol;
    websocket_logger.debug("Sec-WebSocket-Protocol: {}", subprotocol);

    sstring sec_key = req->get_header("Sec-Websocket-Key");
    sstring sec_version = req->get_header("Sec-Websocket-Version");

    sstring sha1_input = sec_key + magic_key_suffix;

    websocket_logger.debug("Sec-Websocket-Key: {}, Sec-Websocket-Version: {}", sec_key, sec_version);

    std::string sha1_output = sha1_base64(sha1_input);
    websocket_logger.debug("SHA1 output: {} of size {}", sha1_output, sha1_output.size());

    co_await _write_buf.write(http_upgrade_reply_template);
    co_await _write_buf.write(sha1_output);
    if (!_subprotocol.empty()) {
        co_await _write_buf.write("\r\nSec-WebSocket-Protocol: ", 26);
        co_await _write_buf.write(_subprotocol);
    }
    co_await _write_buf.write("\r\n\r\n", 4);
    co_await _write_buf.flush();
}

future<> server_connection::read_loop() {
    return read_http_upgrade_request().then([this] {
        return when_all_succeed(
            _handler(_input, _output).handle_exception([this] (std::exception_ptr e) mutable {
                return _read_buf.close().then([e = std::move(e)] () mutable {
                    return make_exception_future<>(std::move(e));
                });
            }),
            do_until([this] {return _done;}, [this] {return read_one();})
        ).discard_result();
    }).finally([this] {
        return _read_buf.close();
    });
}

bool server::is_handler_registered(std::string const& name) {
    return _handlers.find(name) != _handlers.end();
}

void server::register_handler(const std::string& name, handler_t handler) {
    _handlers[name] = handler;
}

}
