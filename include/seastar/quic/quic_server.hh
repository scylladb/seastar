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
 * Copyright (C) 2026 ScyllaDB Ltd.
 */

#pragma once

#include <vector>

#include <seastar/core/shared_ptr.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/sstring.hh>
#include <seastar/quic/quic.hh>

namespace seastar::quic::experimental {

namespace internal {
class quic_server_impl;
class quic_server_shard;

class quic_packet_router {
public:
    virtual ~quic_packet_router() = default;
    virtual future<> route_quic_packet(unsigned shard, socket_address local_address, socket_address src, temporary_buffer<char> packet) = 0;
};
}

/// Listener configuration shared by all connections accepted from this server.
struct quic_server_config {
    /// Local UDP endpoint to bind.
    socket_address listen_address;

    /// PEM certificate chain file used by the server TLS session.
    sstring crt_file;

    /// PEM private key file used by the server TLS session.
    sstring key_file;

    /// Non-empty ALPN protocols advertised during the TLS handshake.
    /// The list and each protocol identifier must be non-empty.
    std::vector<sstring> alpns = {sstring("h3")};

    /// Runtime and transport limits for accepted connections.
    connection_options session_options{};

    /// Allows multiple UDP sockets to bind the same endpoint for kernel fanout.
    bool reuse_port = false;
};

/// Server-side owner of the listening transport and accepted QUIC connections.
class quic_server final {
public:
    /// Constructs a stopped QUIC server.
    quic_server();
    ~quic_server();

    quic_server(quic_server&&) noexcept;
    quic_server& operator=(quic_server&&) noexcept;

    quic_server(const quic_server&) = delete;
    quic_server& operator=(const quic_server&) = delete;

    /// Starts listening for QUIC Initial packets using config.
    future<> start(quic_server_config config);

    /// Waits for the next connection that completed the QUIC and TLS handshakes.
    future<connection> accept();

    /// Returns the local UDP endpoint currently used by the server.
    socket_address local_address() const noexcept;

    /// Stops the listener and all server-owned connections.
    future<> stop();

private:
    friend class internal::quic_server_shard;

    void set_packet_router(internal::quic_packet_router* router) noexcept;
    future<> inject_datagram(socket_address src, temporary_buffer<char> packet);

    lw_shared_ptr<internal::quic_server_impl> _impl;
};

} // namespace seastar::quic::experimental
