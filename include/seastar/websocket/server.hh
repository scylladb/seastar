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

#pragma once

#include <map>

#include <seastar/http/request_parser.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/when_all.hh>
#include <seastar/websocket/common.hh>

namespace seastar::experimental::websocket {

/// \addtogroup websocket
/// @{

/*!
 * \brief a server WebSocket connection
 */
class server_connection : public connection {

    server& _server;
    http_request_parser _http_parser;

public:
    /*!
     * \param server owning \ref server
     * \param fd established socket used for communication
     */
    server_connection(server& server, connected_socket&& fd)
        : connection(std::move(fd))
        , _server(server) {
        on_new_connection();
    }
    ~server_connection();

    /*!
     * \brief serve WebSocket protocol on a server_connection
     */
    future<> process();

protected:
    future<> read_loop();
    future<> read_http_upgrade_request();
    void on_new_connection();
};

/*!
 * \brief a WebSocket server
 *
 * A server capable of establishing and serving connections
 * over WebSocket protocol.
 */
class server {
    std::vector<server_socket> _listeners;
    boost::intrusive::list<server_connection> _connections;
    std::map<std::string, handler_t> _handlers;
    gate _task_gate;
public:
    /*!
     * \brief listen for a WebSocket connection on given address
     * \param addr address to listen on
     */
    void listen(socket_address addr);
    /*!
     * \brief listen for a WebSocket connection on given address with custom options
     * \param addr address to listen on
     * \param lo custom listen options (\ref listen_options)
     */
    void listen(socket_address addr, listen_options lo);

    /*!
     * Stops the server and shuts down all active connections
     */
    future<> stop();

    bool is_handler_registered(std::string const& name);

    /*!
     * \brief Register a handler for specific subprotocol
     * \param name The name of the subprotocol. If it is empty string, then the handler is used
     * when the protocol is not specified
     * \param handler Handler for incoming WebSocket messages.
     */
    void register_handler(const std::string& name, handler_t handler);

    friend class server_connection;
protected:
    void accept(server_socket &listener);
    future<stop_iteration> accept_one(server_socket &listener);
};

/// }@

}
