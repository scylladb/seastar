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

namespace seastar::quic::experimental {

namespace internal {
class quic_client_impl;
}

/// Configuration for a single outbound QUIC connection attempt.
struct quic_client_config {
    /// Remote UDP endpoint to connect to.
    socket_address remote_address;

    /// Local UDP endpoint to bind, or any address in the remote address family.
    std::optional<socket_address> local_address{};

    /// Server Name Indication value used for TLS certificate validation.
    sstring server_name = "localhost";

    /// Optional CA bundle used to validate the server certificate.
    std::optional<sstring> ca_file{};

    /// Non-empty ALPN protocols offered during the TLS handshake.
    /// The list and each protocol identifier must be non-empty.
    std::vector<sstring> alpns = {sstring("h3")};

    /// Runtime and transport limits for the resulting connection.
    connection_options session_options{};
};

/// Client-side owner of transport state that yields one established connection.
class quic_client final {
public:
    /// Constructs a stopped QUIC client.
    quic_client();
    ~quic_client();

    quic_client(quic_client&&) noexcept;
    quic_client& operator=(quic_client&&) noexcept;

    quic_client(const quic_client&) = delete;
    quic_client& operator=(const quic_client&) = delete;

    /// Opens a QUIC connection using config.
    future<connection> connect(quic_client_config config);

    /// Stops background transport work and closes the UDP channel.
    future<> stop();

private:
    std::unique_ptr<internal::quic_client_impl> _impl;
};

} // namespace seastar::quic::experimental
