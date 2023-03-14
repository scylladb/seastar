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

#pragma once

#include <boost/intrusive/list.hpp>
#include <seastar/net/api.hh>
#include <seastar/http/reply.hh>
#include <seastar/core/iostream.hh>

namespace bi = boost::intrusive;

namespace seastar {

namespace tls { class certificate_credentials; }

namespace http {

struct request;
struct reply;

namespace experimental {

/**
 * \brief Class connection represents an HTTP connection over a given transport
 *
 * Check the demos/http_client_demo.cc for usage example
 */

class connection : public enable_shared_from_this<connection> {
    friend class client;
    using hook_t = bi::list_member_hook<bi::link_mode<bi::auto_unlink>>;

    connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    hook_t _hook;
    future<> _closed;

public:
    /**
     * \brief Create an http connection
     *
     * Construct the connection that will work over the provided \fd transport socket
     *
     */
    connection(connected_socket&& fd);

    /**
     * \brief Send the request and wait for response
     *
     * Sends the provided request to the server, and returns a future that will resolve
     * into the server response.
     *
     * If the request was configured with the set_expects_continue() and the server replied
     * early with some error code, this early reply will be returned back.
     *
     * The returned reply only contains the status and headers. To get the reply body the
     * caller should read it via the input_stream provided by the connection.in() method.
     *
     * \param rq -- request to be sent
     *
     */
    future<reply> make_request(request rq);

    /**
     * \brief Get a reference on the connection input stream
     *
     * The stream can be used to get back the server response body. After the stream is
     * finished, the reply can be additionally updated with trailing headers and chunk
     * extentions
     *
     */
    input_stream<char> in(reply& rep);

    /**
     * \brief Closes the connection
     *
     * Connection must be closed regardless of whether there was an exception making the
     * request or not
     */
    future<> close();

private:
    future<> send_request_head(request& rq);
    future<std::optional<reply>> maybe_wait_for_continue(request& req);
    future<> write_body(request& rq);
    future<reply> recv_reply();
};

/**
 * \brief Factory that provides transport for \ref client
 *
 * This customization point allows callers provide its own transport for client. The
 * client code calls factory when it needs more connections to the server and maintains
 * the pool of re-usable sockets internally
 */

class connection_factory {
public:
    /**
     * \brief Make a \ref connected_socket
     *
     * The implementations of this method should return ready-to-use socket that will
     * be used by \ref client as transport for its http connections
     */
    virtual future<connected_socket> make() = 0;
    virtual ~connection_factory() {}
};

/**
 * \brief Class client wraps communications using HTTP protocol
 *
 * The class allows making HTTP requests and handling replies. It's up to the caller to
 * provide a transport, though for simple cases the class provides out-of-the-box
 * facilities.
 *
 * The main benefit client provides against \ref connection is the transparent support
 * for Keep-Alive transport sockets.
 */

class client {
    using connections_list_t = bi::list<connection, bi::member_hook<connection, typename connection::hook_t, &connection::_hook>, bi::constant_time_size<false>>;

    std::unique_ptr<connection_factory> _new_connections;
    connections_list_t _pool;

    using connection_ptr = seastar::shared_ptr<connection>;

    future<connection_ptr> get_connection();
    future<> put_connection(connection_ptr con, bool can_cache);

    template <typename Fn>
    SEASTAR_CONCEPT( requires std::invocable<Fn, connection&> )
    auto with_connection(Fn&& fn);

public:
    using reply_handler = noncopyable_function<future<>(const reply&, input_stream<char>&& body)>;
    /**
     * \brief Construct a simple client
     *
     * This creates a simple client that connects to provided address via plain (non-TLS)
     * socket
     *
     * \param addr -- host address to connect to
     *
     */
    explicit client(socket_address addr);

    /**
     * \brief Construct a secure client
     *
     * This creates a client that connects to provided address via TLS socket with
     * given credentials. In simple words -- this makes an HTTPS client
     *
     * \param addr -- host address to connect to
     * \param creds -- credentials
     * \param host -- optional host name
     *
     */
    client(socket_address addr, shared_ptr<tls::certificate_credentials> creds, sstring host = {});

    /**
     * \brief Construct a client with connection factory
     *
     * This creates a client that uses factory to get \ref connected_socket that is then
     * used as transport. The client may withdraw more than one socket from the factory and
     * may re-use the sockets on its own
     *
     * \param f -- the factory pointer
     *
     */
    explicit client(std::unique_ptr<connection_factory> f);

    /**
     * \brief Send the request and handle the response
     *
     * Sends the provided request to the server and calls the provided callback to handle
     * the response when it arrives. If the reply's status code is not equals the expected
     * value, the handler is not called and the method resolves with exceptional future.
     * Otherwise returns the handler's future
     *
     * \param req -- request to be sent
     * \param handle -- the response handler
     * \param expected -- the expected reply status code
     *
     */
    future<> make_request(request req, reply_handler handle, reply::status_type expected = reply::status_type::ok);

    /**
     * \brief Closes the client
     *
     * Client must be closed before destruction unconditionally
     */
    future<> close();
};

} // experimental namespace

} // http namespace

} // seastar namespace
