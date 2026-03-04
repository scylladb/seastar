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

#include <seastar/http/response_parser.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/gate.hh>
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
class client_connection : public basic_connection<true> {
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
     */
    client_connection(connected_socket&& fd, sstring resource, sstring host,
                      sstring subprotocol, handler_t handler);

    /*!
     * \brief Run the WebSocket client connection.
     *
     * Performs the HTTP upgrade handshake, then runs the handler
     * concurrently with the frame read/write loops.
     */
    future<> process();
};

/*!
 * \brief a WebSocket client
 *
 * A client capable of establishing a WebSocket connection to a server.
 * Manages the connection lifecycle.
 */
class client {
    std::unique_ptr<client_connection> _conn;
    gate _task_gate;

public:
    /*!
     * \brief Connect to a WebSocket server over plain TCP.
     * \param addr server address
     * \param resource the URI path (e.g., "/ws")
     * \param host the Host header value
     * \param subprotocol optional subprotocol name (empty for none)
     * \param handler application handler
     */
    future<> connect(socket_address addr, sstring resource, sstring host,
                     sstring subprotocol, handler_t handler);

    /*!
     * \brief Connect to a WebSocket server over TLS.
     * \param addr server address
     * \param creds TLS credentials
     * \param resource the URI path (e.g., "/ws")
     * \param host the Host header value
     * \param subprotocol optional subprotocol name (empty for none)
     * \param handler application handler
     */
    future<> connect(socket_address addr,
                     shared_ptr<tls::certificate_credentials> creds,
                     sstring resource, sstring host,
                     sstring subprotocol, handler_t handler);

    /*!
     * \brief Close the client and the underlying connection.
     */
    future<> close();
};

/// @}

}
