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

#include <exception>
#include <random>
#include <seastar/core/future.hh>
#include <seastar/coroutine/all.hh>
#include <seastar/websocket/client.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/loop.hh>
#include <seastar/http/reply.hh>

namespace seastar::experimental::websocket {

using namespace std::string_view_literals;

// refer https://datatracker.ietf.org/doc/html/rfc6455#section-1.3
constexpr auto magic_key_suffix_client = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"sv;

static thread_local std::mt19937 rng(std::random_device{}());

static sstring generate_websocket_key() {
    char raw[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t val = rng();
        std::memcpy(raw + i, &val, sizeof(val));
    }
    return sstring(encode_base64(std::string_view(raw, 16)));
}

template <bool text_frame>
client_connection<text_frame>::client_connection(connected_socket&& fd, sstring resource,
                                     sstring host, sstring subprotocol,
                                     handler_t handler)
    : basic_connection<true, text_frame>(std::move(fd))
    , _resource(std::move(resource))
    , _host(std::move(host))
{
    this->_subprotocol = std::move(subprotocol);
    this->_handler = std::move(handler);
}

template <bool text_frame>
future<> client_connection<text_frame>::send_http_upgrade_request() {
    _websocket_key = generate_websocket_key();
    auto req = fmt::format(
        "GET {} HTTP/1.1\r\n"
        "Host: {}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: {}\r\n"
        "Sec-WebSocket-Version: 13\r\n",
        _resource, _host, _websocket_key);

    if (!this->_subprotocol.empty()) {
        req += fmt::format("Sec-WebSocket-Protocol: {}\r\n", this->_subprotocol);
    }
    req += "\r\n";

    co_await this->_write_buf.write(req);
    co_await this->_write_buf.flush();
}

template <bool text_frame>
future<> client_connection<text_frame>::read_http_upgrade_response() {
    _http_parser.init();
    co_await this->_read_buf.consume(_http_parser);

    if (_http_parser.eof()) {
        throw websocket::exception("Connection closed during HTTP upgrade");
    }

    auto resp = _http_parser.get_parsed_response();
    if (!resp) {
        throw websocket::exception("Failed to parse HTTP upgrade response");
    }

    if (resp->_status != http::reply::status_type::switching_protocols) {
        throw websocket::exception(fmt::format(
            "Server responded with status {} instead of 101",
            static_cast<int>(resp->_status)));
    }

    // Validate Sec-WebSocket-Accept
    auto expected_accept = sha1_base64(
        fmt::format("{}{}", _websocket_key, magic_key_suffix_client));

    sstring actual_accept = resp->get_header("Sec-WebSocket-Accept");
    // Trim leading whitespace
    size_t start = 0;
    while (start < actual_accept.size() && !std::isgraph(actual_accept[start])) {
        ++start;
    }
    // No need to trim trailing whitespace, as parse guarantees there are none.
    if (start != 0) {
        actual_accept = sstring(actual_accept.data() + start, actual_accept.size() - start);
    }

    if (actual_accept != sstring(expected_accept)) {
        throw websocket::exception(fmt::format(
            "Invalid Sec-WebSocket-Accept: expected '{}', got '{}'",
            expected_accept, actual_accept));
    }

    websocket_logger.debug("WebSocket client handshake completed");
}

template <bool text_frame>
future<> client_connection<text_frame>::handshake() {
    co_await send_http_upgrade_request();
    co_await read_http_upgrade_response();
}

template <bool text_frame>
future<> client_connection<text_frame>::process() {
    co_await coroutine::all(
        [this] () -> future<> {
            co_await this->_handler(this->_input, this->_output).handle_exception([this] (std::exception_ptr e) -> future<> {
                co_await this->_read_buf.close();
                std::rethrow_exception(e);
            });
        },
        [this] () -> future<> {
            while (!this->_done) {
                co_await this->read_one();
            }
        },
        [this] () {
            return this->response_loop();
        }
    );
}

template <bool text_frame>
future<> client<text_frame>::connect(socket_address addr, sstring resource, sstring host,
                         sstring subprotocol, handler_t handler) {
    auto fd = co_await seastar::connect(addr);
    _conn = std::make_unique<client_connection<text_frame>>(std::move(fd),
        std::move(resource), std::move(host),
        std::move(subprotocol), std::move(handler));

    co_await _conn->handshake();
    (void)try_with_gate(_task_gate, [this] () -> future<> {
        try {
            co_await _conn->process();
        } catch (...) {
            websocket_logger.debug("WebSocket client processing failed: {}", std::current_exception());
        }
    }).handle_exception_type([] (const gate_closed_exception&) {});
}

template <bool text_frame>
future<> client<text_frame>::connect(socket_address addr,
                         shared_ptr<tls::certificate_credentials> creds,
                         sstring resource, sstring host,
                         sstring subprotocol, handler_t handler) {
    auto fd = co_await tls::connect(creds, addr, tls::tls_options{.server_name = host});
    this->_conn = std::make_unique<client_connection<text_frame>>(std::move(fd),
        std::move(resource), std::move(host),
        std::move(subprotocol), std::move(handler));

    co_await _conn->handshake();
    (void)try_with_gate(_task_gate, [this] () -> future<> {
        try {
            co_await _conn->process();
        } catch (...) {
            websocket_logger.debug("WebSocket client processing failed: {}", std::current_exception());
        }
    }).handle_exception_type([] (const gate_closed_exception&) {});
}

template <bool text_frame>
future<> client<text_frame>::close() {
    if (_conn) {
        co_await _conn->close(true).handle_exception([] (auto) {});
        _conn->shutdown_input();
    }
    co_await _task_gate.close();
}

template class client_connection<false>;
template class client_connection<true>;

template class client<true>;
template class client<false>;

}
