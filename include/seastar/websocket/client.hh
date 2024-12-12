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

#include <map>
#include <functional>
#include <random>

#include <seastar/core/sstring.hh>
#include <seastar/http/request.hh>
#include <seastar/http/connection_factory.hh>
#include <seastar/websocket/common.hh>

namespace seastar::experimental::websocket {

class client;

/// \addtogroup websocket
/// @{

/*!
 * \brief a client WebSocket connection
 */
class client_connection : public connection {
    client& _client;
    sstring _ws_key;
public:
    /*!
     * \param server owning \ref server
     * \param fd established socket used for communication
     */
    client_connection(client& client, connected_socket&& fd, std::string_view ws_key,
                      handler_t handler);
    ~client_connection();

    /*!
     * \brief serve WebSocket protocol on a client_connection
     */
    future<> process();

    /**
     * @brief Send a websocket message to the server
     */
    future<> send_message(temporary_buffer<char> buf, bool flush);

protected:
    friend class client;
    future<> perform_handshake(const http::request& req);
    future<> send_request_head(const http::request& req);
    future<> read_reply();
};

/*!
 * \brief a WebSocket client
 *
 * A client capable of establishing and processing a single concurrent connection
 * on a WebSocket protocol.
 */
class client {
    boost::intrusive::list<client_connection> _connections;
    std::string _subprotocol;
    std::unique_ptr<http::experimental::connection_factory> _new_connections;

    std::random_device _rd_device;
    std::mt19937_64 _random_gen;

    using connection_ptr = seastar::shared_ptr<connection>;

public:
    /**
     * \brief Construct a plaintext client
     *
     * This creates a plaintext client that connects to provided address via non-TLS socket.
     *
     * \param addr -- host address to connect to
     */
    explicit client(socket_address addr);

    /**
     * \brief Construct a secure client
     *
     * This creates a secure client that connects to provided address via TLS socket with
     * given credentials.
     *
     * \param addr -- host address to connect to
     * \param creds -- credentials
     * \param host -- optional host name
     */
    client(socket_address addr, shared_ptr<tls::certificate_credentials> creds, sstring host = {});

    /**
     * \brief Construct a client with connection factory
     *
     * This creates a client that uses factory to get \ref connected_socket that is then
     * used as transport.
     *
     * \param f -- the factory pointer
     */
    explicit client(std::unique_ptr<http::experimental::connection_factory> f);

    /**
     * Starts the process of establishing a Websocket connection
     */
    future<seastar::shared_ptr<client_connection>>
            make_request(http::request rq, const handler_t& handler);

    /*!
     * Stops the client and shuts down active connection, if any
     */
    future<> stop();

    void set_subprotocol(std::string const& subprotocol) { _subprotocol = subprotocol; }

    /**
     * Sets the seed for WebSocket key generation
     */
    void set_seed(std::size_t seed);

    friend class client_connection;
};

/// }@

}
