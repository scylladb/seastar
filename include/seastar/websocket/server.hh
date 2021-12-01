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

#include <seastar/http/request_parser.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sstring.hh>
#include <seastar/net/api.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/when_all.hh>

namespace seastar::experimental::websocket {

class server;
struct reply {
    //TODO: implement
};

/*!
 * \brief an error in handling a WebSocket connection
 */
class exception : std::exception {
    std::string _msg;
public:
    exception(std::string_view msg) : _msg(msg) {}
    const char* what() const noexcept {
        return _msg.c_str();
    }
};

/*!
 * \brief a WebSocket connection
 */
class connection : public boost::intrusive::list_base_hook<> {
    server& _server;
    connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    http_request_parser _http_parser;
    std::unique_ptr<reply> _resp;
    queue<std::unique_ptr<reply>> _replies{10};
    bool _done = false;
public:
    /*!
     * \param server owning \ref server
     * \param fd established socket used for communication
     */
    connection(server& server, connected_socket&& fd)
        : _server(server)
        , _fd(std::move(fd))
        , _read_buf(_fd.input())
        , _write_buf(_fd.output())
    {
        on_new_connection();
    }
    ~connection();

    /*!
     * \brief serve WebSocket protocol on a connection
     */
    future<> process();
    /*!
     * \brief close the socket
     */
    void shutdown();

protected:
    future<> read_loop();
    future<> read_one();
    future<> read_http_upgrade_request();
    future<> response_loop();
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
    gate _task_gate;
    boost::intrusive::list<connection> _connections;
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

    friend class connection;
protected:
    void do_accepts(int which);
    future<> do_accept_one(int which);
};

}
