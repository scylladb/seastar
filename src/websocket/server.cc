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
#include <seastar/util/log.hh>
#include <cryptopp/sha.h>
#include <cryptopp/filters.h>
#include <cryptopp/base64.h>

#ifndef CRYPTOPP_NO_GLOBAL_BYTE
namespace CryptoPP {
using byte = unsigned char;
}
#endif

namespace seastar::experimental::websocket {

static sstring magic_key_suffix = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static sstring http_upgrade_reply_template =
    "HTTP/1.1 101 Switching Protocols\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Accept: ";

static logger wlogger("websocket");

void server::listen(socket_address addr, listen_options lo) {
    _listeners.push_back(seastar::listen(addr, lo));
    do_accepts(_listeners.size() - 1);
}
void server::listen(socket_address addr) {
    listen_options lo;
    lo.reuse_address = true;
    return listen(addr, lo);
}

void server::do_accepts(int which) {
    // Waited on with the gate
    (void)try_with_gate(_task_gate, [this, which] {
        return keep_doing([this, which] {
            return try_with_gate(_task_gate, [this, which] {
                return do_accept_one(which);
            });
        }).handle_exception_type([](const gate_closed_exception& e) {});
    }).handle_exception_type([](const gate_closed_exception& e) {});
}

future<> server::do_accept_one(int which) {
    return _listeners[which].accept().then([this] (accept_result ar) mutable {
        auto conn = std::make_unique<connection>(*this, std::move(ar.connection));
        (void)try_with_gate(_task_gate, [conn = std::move(conn)]() mutable {
            return conn->process().handle_exception([conn = std::move(conn)] (std::exception_ptr ex) {
                wlogger.error("request error: {}", ex);
            });
        }).handle_exception_type([] (const gate_closed_exception& e) {});
    }).handle_exception_type([] (const std::system_error &e) {
        // We expect a ECONNABORTED when server::stop is called,
        // no point in warning about that.
        if (e.code().value() != ECONNABORTED) {
            wlogger.error("accept failed: {}", e);
        }
    }).handle_exception([] (std::exception_ptr ex) {
        wlogger.error("accept failed: {}", ex);
    });
}

future<> server::stop() {
    future<> tasks_done = _task_gate.close();
    for (auto&& l : _listeners) {
        l.abort_accept();
    }
    for (auto&& c : _connections) {
        c.shutdown();
    }
    return tasks_done;
}

connection::~connection() {
    _server._connections.erase(_server._connections.iterator_to(*this));
}

void connection::on_new_connection() {
    _server._connections.push_back(*this);
}

future<> connection::process() {
    return when_all(read_loop(), response_loop()).then(
            [] (std::tuple<future<>, future<>> joined) {
        try {
            std::get<0>(joined).get();
        } catch (...) {
            wlogger.debug("Read exception encountered: {}", std::current_exception());
        }
        try {
            std::get<1>(joined).get();
        } catch (...) {
            wlogger.debug("Response exception encountered: {}", std::current_exception());
        }
        return make_ready_future<>();
    });
}

static std::string sha1_base64(std::string_view source) {
    // CryptoPP insists on freeing the pointers by itself...
    // It's leaky, but `read_http_upgrade_request` is a one-shot operation
    // per handshake, so the real risk is not particularly great.
    CryptoPP::SHA1 sha1;
    std::string hash;
    CryptoPP::StringSource(reinterpret_cast<const CryptoPP::byte*>(source.data()), source.size(),
            true, new CryptoPP::HashFilter(sha1, new CryptoPP::Base64Encoder(new CryptoPP::StringSink(hash), false)));
    return hash;
}

future<> connection::read_http_upgrade_request() {
    _http_parser.init();
    return _read_buf.consume(_http_parser).then([this] () mutable {
        if (_http_parser.eof()) {
            _done = true;
            return make_ready_future<>();
        }
        std::unique_ptr<httpd::request> req = _http_parser.get_parsed_request();
        if (_http_parser.failed()) {
            throw websocket::exception("Incorrect upgrade request");
        }

        sstring upgrade_header = req->get_header("Upgrade");
        if (upgrade_header != "websocket") {
            throw websocket::exception("Upgrade header missing");
        }
        sstring sec_key = req->get_header("Sec-Websocket-Key");
        sstring sec_version = req->get_header("Sec-Websocket-Version");

        sstring sha1_input = sec_key + magic_key_suffix;

        wlogger.debug("Sec-Websocket-Key: {}, Sec-Websocket-Version: {}", sec_key, sec_version);

        std::string sha1_output = sha1_base64(sha1_input);
        wlogger.debug("SHA1 output: {} of size {}", sha1_output, sha1_output.size());

        return _write_buf.write(http_upgrade_reply_template).then([this, sha1_output = std::move(sha1_output)] {
            return _write_buf.write(sha1_output);
        }).then([this] {
            return _write_buf.write("\r\n\r\n", 4);
        }).then([this] {
            return _write_buf.flush();
        });
    });
}

future<> connection::read_one() {
    return _read_buf.read().then([this] (temporary_buffer<char> buf) {
        if (buf.empty()) {
            _done = true;
        }
        //FIXME: implement
        wlogger.info("Received: {}", buf.get());
    });
}

future<> connection::read_loop() {
    return read_http_upgrade_request().then([this] {
        return do_until([this] {return _done;}, [this] {
            return read_one();
        });
    }).then_wrapped([this] (future<> f) {
        if (f.failed()) {
            wlogger.error("Read failed: {}", f.get_exception());
        }
        return _replies.push_eventually({});
    }).finally([this] {
        return _read_buf.close();
    });
}

future<> connection::response_loop() {
    // FIXME: implement
    return make_ready_future<>();
}

void connection::shutdown() {
    wlogger.debug("Shutting down");
    _fd.shutdown_input();
    _fd.shutdown_output();
}

}
