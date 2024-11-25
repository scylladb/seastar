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

#ifndef SEASTAR_MODULE
#include <boost/intrusive/list.hpp>
#endif
#include <seastar/net/api.hh>
#include <seastar/http/connection_factory.hh>
#include <seastar/http/reply.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/iostream.hh>
#include <seastar/util/modules.hh>

namespace bi = boost::intrusive;

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

namespace tls { class certificate_credentials; }

namespace http {

namespace experimental { class client; }
struct request;
struct reply;

namespace internal {

class client_ref {
    http::experimental::client* _c;
public:
    client_ref(http::experimental::client* c) noexcept;
    ~client_ref();
    client_ref(client_ref&& o) noexcept : _c(std::exchange(o._c, nullptr)) {}
    client_ref(const client_ref&) = delete;
};

}

namespace experimental {

/**
 * \brief Class connection represents an HTTP connection over a given transport
 *
 * Check the demos/http_client_demo.cc for usage example
 */

class connection : public enable_shared_from_this<connection> {
    friend class client;
    using hook_t = bi::list_member_hook<bi::link_mode<bi::auto_unlink>>;
    using reply_ptr = std::unique_ptr<reply>;

    connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    hook_t _hook;
    future<> _closed;
    internal::client_ref _ref;
    // Client sends HTTP-1.1 version and assumes the server is 1.1-compatible
    // too and thus the connection will be persistent by default. If the server
    // responds with older version, this flag will be dropped (see recv_reply())
    bool _persistent = true;

public:
    /**
     * \brief Create an http connection
     *
     * Construct the connection that will work over the provided \fd transport socket
     *
     */
    connection(connected_socket&& fd, internal::client_ref cr);

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
    future<reply_ptr> do_make_request(request& rq);
    void setup_request(request& rq);
    future<> send_request_head(const request& rq);
    future<reply_ptr> maybe_wait_for_continue(const request& req);
    future<> write_body(const request& rq);
    future<reply_ptr> recv_reply();

    void shutdown() noexcept;
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
public:
    using reply_handler = noncopyable_function<future<>(const reply&, input_stream<char>&& body)>;
    using retry_requests = bool_class<struct retry_requests_tag>;

private:
    friend class http::internal::client_ref;
    using connections_list_t = bi::list<connection, bi::member_hook<connection, typename connection::hook_t, &connection::_hook>, bi::constant_time_size<false>>;
    static constexpr unsigned default_max_connections = 100;

    std::unique_ptr<connection_factory> _new_connections;
    unsigned _nr_connections = 0;
    unsigned _max_connections;
    unsigned long _total_new_connections = 0;
    const retry_requests _retry;
    condition_variable _wait_con;
    connections_list_t _pool;

    using connection_ptr = seastar::shared_ptr<connection>;

    future<connection_ptr> get_connection(abort_source* as);
    future<connection_ptr> make_connection(abort_source* as);
    future<> put_connection(connection_ptr con);
    future<> shrink_connections();

    template <std::invocable<connection&> Fn>
    auto with_connection(Fn&& fn, abort_source*);

    template <typename Fn>
    requires std::invocable<Fn, connection&>
    auto with_new_connection(Fn&& fn, abort_source*);

    future<> do_make_request(request& req, reply_handler& handle, abort_source*, std::optional<reply::status_type> expected);
    future<> do_make_request(connection& con, request& req, reply_handler& handle, abort_source*, std::optional<reply::status_type> expected);

public:
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
     * \param max_connections -- maximum number of connection a client is allowed to maintain
     * (both active and cached in pool)
     * \param retry -- whether or not to retry requests on connection IO errors
     *
     * The client uses connections provided by factory to send requests over and receive responses
     * back. Once request-response cycle is over the connection used for that is kept by a client
     * in a "pool". Making another http request may then pick up the existing connection from the
     * pool thus avoiding the extra latency of establishing new connection. Pool may thus accumulate
     * more than one connection if user sends several requests in parallel.
     *
     * HTTP servers may sometimes want to terminate the connections it keeps. This can happen in
     * one of several ways.
     *
     * The "gentle" way is when server adds the "connection: close" header to its response. In that
     * case client would handle the response and will just close the connection without putting it
     * to pool.
     *
     * Less gentle way a server may terminate a connection is by closing it, so the underlying TCP
     * stack would communicate regular TCP FIN-s. If the connection happens to be in pool when it
     * happens the client would just clean the connection from pool in the background.
     *
     * Sometimes the least gentle closing occurs when server closes the connection on the fly and
     * TCP starts communicating FIN-s in parallel with client using it. In that case, user would
     * receive exception from the \ref make_request() call and will have to do something about it.
     * Client provides a transparent way of handling it called "retry".
     *
     * When enabled, it makes client catch the transport error, close the broken connection, open
     * another one and retry the very same request one more time over this new connection. If the
     * second attempt fails, this error is reported back to user.
     */
    explicit client(std::unique_ptr<connection_factory> f, unsigned max_connections = default_max_connections, retry_requests retry = retry_requests::no);

    /**
     * \brief Send the request and handle the response
     *
     * Sends the provided request to the server and calls the provided callback to handle
     * the response when it arrives. If the expected status is specified and the response's
     * status is not the expected one, the handler is not called and the method resolves
     * with exceptional future. Otherwise returns the handler's future
     *
     * \param req -- request to be sent
     * \param handle -- the response handler
     * \param expected -- the optional expected reply status code, default is std::nullopt
     *
     * Note that the handle callback should be prepared to be called more than once, because
     * client may restart the whole request processing in case server closes the connection
     * in the middle of operation
     */
    future<> make_request(request&& req, reply_handler&& handle, std::optional<reply::status_type>&& expected = std::nullopt);

    /**
     * \brief Send the request and handle the response (abortable)
     *
     * Same as previous method, but aborts the request upon as.request_abort() call
     *
     * \param req -- request to be sent
     * \param handle -- the response handler
     * \param as -- abort source that aborts the request
     * \param expected -- the optional expected reply status code, default is std::nullopt
     */
    future<> make_request(request&& req, reply_handler&& handle, abort_source& as, std::optional<reply::status_type>&& expected = std::nullopt);

    /**
     * \brief Send the request and handle the response, same as \ref make_request()
     *
     *  @attention Note that the method does not take the ownership of the
     * `request and the `handle`, it caller's responsibility the make sure they
     * are referencing valid instances
     */
    future<> make_request(request& req, reply_handler& handle, std::optional<reply::status_type> expected = std::nullopt);

    /**
     * \brief Send the request and handle the response (abortable), same as \ref make_request()
     *
     *  @attention Note that the method does not take the ownership of the
     * `request and the `handle`, it caller's responsibility the make sure they
     * are referencing valid instances
     */
    future<> make_request(request& req, reply_handler& handle, abort_source& as, std::optional<reply::status_type> expected = std::nullopt);

    /**
     * \brief Updates the maximum number of connections a client may have
     *
     * If the new limit is less than the amount of connections a client has, they will be
     * closed. The returned future resolves when all excessive connections get closed
     *
     * \param nr -- the new limit on the number of connections
     */
    future<> set_maximum_connections(unsigned nr);

    /**
     * \brief Closes the client
     *
     * Client must be closed before destruction unconditionally
     */
    future<> close();

    /**
     * \brief Returns the total number of connections
     */

    unsigned connections_nr() const noexcept {
        return _nr_connections;
    }

    /**
     * \brief Returns the number of idle connections
     */

    unsigned idle_connections_nr() const noexcept {
        return _pool.size();
    }

    /**
     * \brief Returns the total number of connection factory invocations made so far
     *
     * This is the monotonically-increasing counter describing how "frequently" the
     * client kicks its factory for new connections.
     */

    unsigned long total_new_connections_nr() const noexcept {
        return _total_new_connections;
    }
};

} // experimental namespace

} // http namespace

SEASTAR_MODULE_EXPORT_END
} // seastar namespace
