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
 * Copyright (C) 2026 Kefu Chai (tchaikov@gmail.com)
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <memory>
#include <seastar/core/future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/http/routes.hh>
#include <seastar/net/quic.hh>
#include <seastar/net/socket_defs.hh>
#endif

namespace seastar::http::experimental {

/// \defgroup http3 HTTP/3 Server
///
/// Seastar's HTTP/3 server layer, built on the QUIC transport
/// (`seastar::experimental::quic`) and nghttp3.
///
/// HTTP/3 requests are dispatched through the same `httpd::routes`
/// infrastructure used by the HTTP/1.1 server — handlers are
/// protocol-agnostic.
///
/// \addtogroup http3
/// @{

/// An HTTP/3 server that listens on a UDP socket, accepts QUIC connections,
/// and dispatches HTTP/3 requests through `httpd::routes`.
///
/// Each shard should create its own server on the same address
/// (using `SO_REUSEPORT` via the underlying QUIC server).
///
/// Usage:
/// \code
///   httpd::routes routes;
///   routes.add(httpd::operation_type::GET, httpd::url("/"), handler);
///
///   auto creds = make_shared<quic::credentials>();
///   creds->set_x509_key_file("cert.crt", "key.pem");
///   creds->set_alpn("h3");
///
///   auto srv = co_await http3_server::listen(addr, creds, routes);
///   // ... accept loop runs in the background ...
///   co_await srv.stop();
/// \endcode
class http3_server {
public:
    http3_server(http3_server&&) noexcept;
    http3_server& operator=(http3_server&&) noexcept;
    ~http3_server();

    /// Start listening for HTTP/3 connections on the given address.
    ///
    /// \param addr     Local UDP address to bind.
    /// \param creds    TLS credentials (must have cert + key; ALPN should be
    ///                 "h3" for interoperability).
    /// \param routes   Request router shared by all connections on this shard.
    /// \param config   QUIC transport configuration.
    static future<http3_server> listen(
        socket_address addr,
        shared_ptr<seastar::experimental::quic::credentials> creds,
        httpd::routes& routes,
        seastar::experimental::quic::connection_config config = {});

    /// Stop accepting new connections and gracefully close the server.
    future<> stop();

    /// The address this server is bound to.
    socket_address local_address() const noexcept;

private:
    class impl;
    explicit http3_server(std::unique_ptr<impl> p);
    std::unique_ptr<impl> _impl;
};

/// @}

} // namespace seastar::http::experimental
