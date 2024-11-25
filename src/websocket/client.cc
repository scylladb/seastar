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

#include <map>
#include <functional>

#include <seastar/core/sstring.hh>
#include <seastar/core/when_all.hh>
#include <seastar/net/tls.hh>
#include <seastar/http/response_parser.hh>
#include <seastar/websocket/common.hh>
#include <seastar/websocket/client.hh>
#include <gnutls/gnutls.h>

namespace seastar::experimental::websocket {

client_connection::client_connection(client& client, connected_socket&& fd, std::string_view ws_key,
                                     const handler_t handler)
        : connection(std::move(fd), true)
        , _client{client}
        , _ws_key{ws_key} {
    _handler = std::move(handler);
    _client._connections.push_back(*this);
}

client_connection::~client_connection() {
    _client._connections.erase(_client._connections.iterator_to(*this));
}

future<> client_connection::perform_handshake(const http::request& req) {
    return send_request_head(req).then(
            [this]{ return read_reply(); }
    ).handle_exception([this](auto ep) {
        websocket_logger.error("Got error during handshake {}", ep);
        return _read_buf.close();
    });
}

future<> client_connection::send_request_head(const http::request& req) {
    return _write_buf.write(req.request_line()).then([this, &req] {
        return req.write_request_headers(_write_buf).then([this] {
            return _write_buf.write("\r\n", 2);
        }).then([this] {
            return _write_buf.flush();
        });
    });
}

future<> client_connection::process() {
    return when_all_succeed(
        _handler(_input, _output).handle_exception([this] (std::exception_ptr e) mutable {
            return _read_buf.close().then([e = std::move(e)] () mutable {
                return make_exception_future<>(std::move(e));
            });
        }),
        response_loop(),
        do_until([this] {return _done;}, [this] {return read_one();})
    ).discard_result().finally([this] {
        return _read_buf.close();
    });
}

future<> client_connection::send_message(temporary_buffer<char> buf, bool flush) {
    auto f = _output.write(std::move(buf));
    if (flush) {
        f = f.then([this](){ return _output.flush(); });
    }
    return f;
}

future<> client_connection::read_reply() {
    http_response_parser parser;
    return do_with(std::move(parser), [this] (auto& parser) {
        parser.init();
        return _read_buf.consume(parser).then([this, &parser] {
            if (parser.eof()) {
                websocket_logger.trace("Parsing response EOFed");
                throw std::system_error(ECONNABORTED, std::system_category());
            }
            if (parser.failed()) {
                websocket_logger.trace("Parsing response failed");
                throw std::runtime_error("Invalid http server response");
            }

            std::unique_ptr<http::reply> resp = parser.get_parsed_response();
            if (resp->_status != http::reply::status_type::switching_protocols) {
                websocket_logger.trace("Didn't receive 101 switching protocols response");
                throw std::runtime_error("Invalid http server response");
            }

            if (resp->get_header("Upgrade").find("websocket") == sstring::npos) {
                websocket_logger.trace("Bad or non-existing Upgrade header");
                throw std::runtime_error("Bad or non-existing Upgrade header");
            }
            if (resp->get_header("Connection").find("Upgrade") == sstring::npos) {
                websocket_logger.trace("Bad or non-existing Connection header");
                throw std::runtime_error("Bad or non-existing Connection header");
            }
            auto accept = resp->get_header("Sec-WebSocket-Accept");
            if (accept.empty()) {
                websocket_logger.trace("Did not receive Sec-WebSocket-Accept header");
                throw std::runtime_error("Did not receive Sec-WebSocket-Accept header");
            }
            if (accept != sha1_base64(_ws_key + magic_key_suffix)) {
                websocket_logger.trace("Received mismatching Sec-WebSocket-Accept header");
                throw std::runtime_error("Received mismatching Sec-WebSocket-Accept header");
            }

            return make_ready_future<>();
        });
    });
}

client::client(socket_address addr)
    : client(std::make_unique<http::experimental::basic_connection_factory>(
                std::move(addr)))
{
}

client::client(socket_address addr, shared_ptr<tls::certificate_credentials> creds, sstring host)
        : client(std::make_unique<http::experimental::tls_connection_factory>(
                std::move(addr), std::move(creds), std::move(host)))
{
}
client::client(std::unique_ptr<http::experimental::connection_factory> f)
    : _new_connections(std::move(f))
    , _random_gen{_rd_device()}
{
}

future<seastar::shared_ptr<client_connection>>
        client::make_request(http::request rq, const handler_t& handler) {
    if (rq._version.empty()) {
        rq._version = "1.1";
    }
    rq._headers["Upgrade"] = "websocket";
    rq._headers["Connection"] = "Upgrade";
    rq._headers["Sec-WebSocket-Version"] = "13";
    if (!_subprotocol.empty()) {
        rq._headers["Sec-WebSocket-Protocol"] = _subprotocol;
    }

    uint8_t key[16] = {};
    std::uniform_int_distribution dist(0, 255);
    for (auto& key_char : key) {
        key_char = dist(_random_gen);
    }

    std::string ws_key = encode_base64(std::string_view(reinterpret_cast<char*>(key), sizeof(key)));
    rq._headers["Sec-WebSocket-Key"] = ws_key;

    abort_source* as = nullptr; // TODO

    return do_with(std::move(rq), [this, as, handler, ws_key](auto& rq) {
        return _new_connections->make(as).then([this, &rq, as, handler, ws_key] (connected_socket cs) {
            websocket_logger.trace("created new http connection {}", cs.local_address());

            auto con = seastar::make_shared<client_connection>(*this, std::move(cs), ws_key, handler);

            auto sub = as ? as->subscribe([con] () noexcept { con->shutdown_input(); }) : std::nullopt;
            return con->perform_handshake(rq).then([con](){ return con; });
        });
    });
}

future<> client::stop() {
    for (auto&& c : _connections) {
        c.shutdown_input();
    }

    return parallel_for_each(_connections, [] (client_connection& conn) {
        return conn.close(true).handle_exception([] (auto ignored) {});
    });
}

void client::set_seed(std::size_t seed) {
    _random_gen.seed(seed);
}

}
