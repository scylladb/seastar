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

#include <memory>
#include <optional>
#include <vector>

#include <seastar/core/sstring.hh>
#include <seastar/quic/quic.hh>

/// \brief Client-side entry points for the experimental QUIC transport.
namespace seastar::quic::experimental {

/// \cond internal
namespace internal {
class quic_client_impl;
}
/// \endcond

/// \brief Configuration for a single outbound QUIC connection attempt.
struct quic_client_config {
    /// \brief Remote UDP endpoint to connect to.
    socket_address remote_address;

    /// \brief Local UDP endpoint to bind.
    ///
    /// When unset, the client binds a wildcard address in the remote endpoint's
    /// address family and lets the operating system choose the source port.
    std::optional<socket_address> local_address{};

    /// \brief DNS name sent as TLS Server Name Indication and verified in the certificate.
    ///
    /// An empty value disables SNI and hostname matching, but not certificate
    /// chain verification.
    sstring server_name = "localhost";

    /// \brief Additional PEM CA bundle used to validate the server certificate.
    ///
    /// The system trust store is loaded whether or not this value is set.
    std::optional<sstring> ca_file{};

    /// \brief Non-empty ALPN protocols offered during the TLS handshake.
    /// The list and each protocol identifier must be non-empty.
    std::vector<sstring> alpns = {sstring("h3")};

    /// \brief Runtime and transport limits for the resulting connection.
    connection_options session_options{};
};

/// \brief Client-side owner of one UDP transport and one QUIC connection.
///
/// A client instance supports one active connect() attempt or connection at a
/// time. Call stop() before reusing or destroying an active client.
class quic_client final {
public:
    /// \brief Construct a stopped QUIC client.
    quic_client();

    /// \brief Destroy the client and its implementation state.
    ~quic_client();

    /// \brief Move a client, including any active transport state.
    quic_client(quic_client&&) noexcept;

    /// \brief Replace this client with another client.
    quic_client& operator=(quic_client&&) noexcept;

    /// \brief Clients cannot be copied.
    quic_client(const quic_client&) = delete;

    /// \brief Clients cannot be copy-assigned.
    quic_client& operator=(const quic_client&) = delete;

    /// \brief Establish a QUIC connection.
    ///
    /// The future resolves only after the QUIC and TLS handshakes complete and
    /// ALPN is negotiated.
    ///
    /// \param config Remote endpoint, TLS identity, ALPN list, and transport options.
    /// \return A future containing the established connection. It fails with
    ///         quic_error if the configuration is invalid, a connection is
    ///         already active, certificate validation fails, or the handshake
    ///         fails.
    future<connection> connect(quic_client_config config);

    /// \brief Stop background transport work and close the UDP channel.
    ///
    /// Active streams and pending operations are failed. The operation is
    /// idempotent.
    future<> stop();

private:
    std::unique_ptr<internal::quic_client_impl> _impl;
};

} // namespace seastar::quic::experimental
