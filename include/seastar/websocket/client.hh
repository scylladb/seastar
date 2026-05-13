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

#pragma once

#include <seastar/core/future.hh>
#include <seastar/http/response_parser.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/gate.hh>
#include <seastar/net/api.hh>
#include <seastar/net/tls.hh>
#include <seastar/websocket/common.hh>

namespace seastar::experimental::websocket {

/// \addtogroup websocket
/// @{

/*!
 * \brief a client WebSocket connection
 *
 * Represents a single WebSocket connection initiated by a client.
 * Handles the HTTP upgrade handshake and WebSocket frame exchange.
 */
template <bool text_frame = false>
class client_connection : public basic_connection<true, text_frame> {
    http_response_parser _http_parser;
    sstring _resource;
    sstring _host;
    sstring _websocket_key;

    future<> send_http_upgrade_request();
    future<> read_http_upgrade_response();

public:
    /*!
     * \param fd established socket used for communication
     * \param resource the URI path (e.g., "/ws")
     * \param host the Host header value
     * \param subprotocol optional subprotocol name
     * \param handler application handler for incoming/outgoing data
     * \param options connection tuning options.
     */
    client_connection(connected_socket&& fd, sstring resource, sstring host,
                      sstring subprotocol, handler_t handler,
                      connection_options options = {});

    /*!
     * \brief Run the WebSocket client connection.
     */
    future<> process();

    /*!
     * \brief Perform the WebSocket opening handshake.
     *
     * Sends an HTTP Upgrade request to the server and waits for
     * a valid HTTP 101 Switching Protocols response. Upon success,
     * the connection is upgraded to the WebSocket protocol and is
     * ready for frame-level communication.
     *
     * \throws websocket::exception if the server rejects the upgrade
     *         or returns an invalid handshake response.
     */
    future<> handshake();
};

/*!
 * \brief a WebSocket client
 *
 * A client capable of establishing a WebSocket connection to a server.
 * Manages the connection lifecycle.
 */
template<bool text_frame = false>
class client {
    std::unique_ptr<client_connection<text_frame>> _conn;
    gate _task_gate;
    bool _handshake_done = false;
    future<> connect(connected_socket fd, sstring resource, sstring host,
                     sstring subprotocol, handler_t handler,
                     const connection_options& options);
public:
    /*!
     * \brief Connect to a WebSocket server over plain TCP.
     * \param addr server address
     * \param resource the URI path (e.g., "/ws")
     * \param host the Host header value
     * \param subprotocol optional subprotocol name (empty for none)
     * \param handler application handler
     * \param options connection tuning options.
     */
    future<> connect(socket_address addr, sstring resource, sstring host,
                     sstring subprotocol, handler_t handler,
                     connection_options options = {});

    /*!
     * \brief Connect to a WebSocket server over TLS.
     * \param addr server address
     * \param creds TLS credentials
     * \param resource the URI path (e.g., "/ws")
     * \param host the Host header value
     * \param subprotocol optional subprotocol name (empty for none)
     * \param handler application handler
     * \param options connection tuning options.
     */
    future<> connect(socket_address addr,
                     shared_ptr<tls::certificate_credentials> creds,
                     sstring resource, sstring host,
                     sstring subprotocol, handler_t handler,
                     connection_options options = {});

    /*!
     * \brief Close the connection and wait for the client processing task to
     *        finish.
     *
     * The client handler owns the WebSocket connection lifetime. To close the
     * connection normally, the handler should call close() on the output stream
     * passed to it.
     *
     * close() initiates a WebSocket CLOSE handshake and tears down the
     * underlying streams. A handler blocked in input_stream::read() is unblocked
     * because the stream teardown shuts down the socket read side (SHUT_RD),
     * causing the pending read to observe EOF.
     *
     * The one case where this future may not resolve is when the CLOSE frame
     * itself cannot be sent (for example, the peer's TCP receive window is full
     * and the outbound write never yields back). In that case the teardown
     * path never runs, and shutdown() can be used as an out-of-band escape
     * hatch: it directly shuts down the connection's input so the handler can
     * return and the task gate can drain.
     *
     * \sa shutdown()
     */
    future<> close();
    /*!
     * \brief Shut down the underlying connection input as an out-of-band escape.
     *
     * Synchronously shuts down the connection's read side without going through
     * the CLOSE handshake. Useful when close() cannot make progress because the
     * outbound path is stalled (peer recv-window full, dead peer holding the
     * connection open). Calling shutdown() unblocks a handler waiting in
     * input_stream::read() so close() and the task gate can finish.
     *
     * Normal teardown should go through close(); shutdown() is an escape hatch
     * for outbound-stall scenarios.
     *
     * \sa close()
     */
    void shutdown();
};

/// @}

}
