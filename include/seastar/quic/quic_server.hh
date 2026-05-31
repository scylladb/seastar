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

#include <array>
#include <vector>

#include <seastar/core/shared_ptr.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/sstring.hh>
#include <seastar/quic/quic.hh>

/// \brief Server-side entry points for the experimental QUIC transport.
namespace seastar::quic::experimental {

/// \cond internal
namespace internal {
class quic_server_impl;
class quic_server_shard;

using quic_server_cid_key = std::array<uint8_t, 32>;

class quic_packet_router {
public:
    virtual ~quic_packet_router() = default;
    virtual future<> route_quic_packet(unsigned shard, socket_address local_address, socket_address src, temporary_buffer<char> packet) = 0;
};
}
/// \endcond

/// \brief Listener configuration shared by all connections accepted from this server.
struct quic_server_config {
    /// \brief Local UDP endpoint to bind.
    socket_address listen_address;

    /// \brief PEM certificate chain file used by the server TLS session.
    sstring crt_file;

    /// \brief PEM private key file used by the server TLS session.
    sstring key_file;

    /// \brief Non-empty ALPN protocols advertised during the TLS handshake.
    /// The list and each protocol identifier must be non-empty.
    std::vector<sstring> alpns = {sstring("h3")};

    /// \brief Runtime and transport limits for accepted connections.
    connection_options session_options{};
};

/// \brief Shard-local QUIC listener and owner of accepted connections.
///
/// This low-level server binds one UDP socket on the current shard. Applications
/// that run on more than one shard should normally use sharded_quic_server,
/// which creates one listener per shard with `SO_REUSEPORT` and routes packets
/// to the shard that owns their connection.
class quic_server final {
public:
    /// \brief Construct a stopped QUIC server.
    quic_server();

    /// \brief Destroy the server and request detached cleanup if it is still running.
    ~quic_server();

    /// \brief Move a shard-local server.
    quic_server(quic_server&&) noexcept;

    /// \brief Replace this server with another shard-local server.
    quic_server& operator=(quic_server&&) noexcept;

    /// \brief Servers cannot be copied.
    quic_server(const quic_server&) = delete;

    /// \brief Servers cannot be copy-assigned.
    quic_server& operator=(const quic_server&) = delete;

    /// \brief Bind the configured UDP endpoint and begin receiving QUIC packets.
    ///
    /// \param config Listen endpoint, TLS credentials, ALPN list, and per-connection
    ///               options.
    /// \return A future that fails with quic_error if the server is already
    ///         started, the configuration is invalid, credentials cannot be
    ///         loaded, or the endpoint cannot be bound.
    future<> start(quic_server_config config);

    /// \brief Wait for the next fully established connection.
    ///
    /// \return A future containing the next connection after its QUIC and TLS
    ///         handshakes complete and ALPN is negotiated. The future fails when
    ///         the server stops or its receive loop fails.
    future<connection> accept();

    /// \brief Get the bound UDP endpoint.
    ///
    /// \return The effective endpoint, including an automatically selected port,
    ///         or an empty address before start() and after stop().
    socket_address local_address() const noexcept;

    /// \brief Stop accepting packets and close all server-owned connections.
    ///
    /// The operation waits for background work to finish and is idempotent.
    future<> stop();

private:
    friend class internal::quic_server_shard;

    future<> start_shard(
            quic_server_config config,
            internal::quic_server_cid_key cid_key,
            bool reuse_port);
    void set_packet_router(internal::quic_packet_router* router) noexcept;
    future<> inject_datagram(socket_address src, temporary_buffer<char> packet);

    lw_shared_ptr<internal::quic_server_impl> _impl;
};

} // namespace seastar::quic::experimental
