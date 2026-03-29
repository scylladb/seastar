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
#include <cstdint>
#include <memory>
#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/timer.hh>
#include <seastar/net/api.hh>
#include <seastar/net/socket_defs.hh>
#endif

namespace seastar::experimental::quic {

/// \defgroup quic QUIC Transport
///
/// Seastar's QUIC transport layer built on ngtcp2 (QUIC) and nghttp3
/// (HTTP/3).  All types live in `seastar::experimental::quic`.
///
/// \addtogroup quic
/// @{

class connection;
class server;
class stream;

// Implementation classes (defined in quic.cc).
class stream_impl;
class connection_impl;
class credentials_impl;

/// Configuration for a QUIC connection.
struct connection_config {
    /// Maximum idle timeout in milliseconds. 0 means use the library default.
    uint64_t max_idle_timeout_ms = 30000;
    /// Initial max data the peer may send (connection-level flow control).
    uint64_t initial_max_data = 10 * 1024 * 1024;
    /// Initial max data per bidirectional stream.
    uint64_t initial_max_stream_data_bidi_local = 256 * 1024;
    /// Initial max data per remote-initiated bidirectional stream.
    uint64_t initial_max_stream_data_bidi_remote = 256 * 1024;
    /// Initial max data per unidirectional stream.
    uint64_t initial_max_stream_data_uni = 256 * 1024;
    /// Maximum number of concurrent bidirectional streams the peer can open.
    uint64_t initial_max_streams_bidi = 100;
    /// Maximum number of concurrent unidirectional streams the peer can open.
    uint64_t initial_max_streams_uni = 3;
};

/// TLS credentials for QUIC connections.
///
/// Manages X.509 certificates, private keys, and trust anchors used
/// during the QUIC/TLS 1.3 handshake.  A single credentials object
/// can be shared by many connections.
class credentials {
public:
    credentials();
    ~credentials();
    credentials(credentials&&) noexcept;
    credentials& operator=(credentials&&) noexcept;

    /// Load an X.509 certificate chain and private key from PEM files.
    void set_x509_key_file(const std::string& cert_file,
                           const std::string& key_file);

    /// Set the trusted CA certificate bundle (PEM format) for
    /// verifying the peer's certificate.
    void set_x509_trust_file(const std::string& ca_file);

    /// Disable certificate verification (for testing only).
    void set_no_verify();

    /// Set the ALPN token advertised during the TLS 1.3 handshake,
    /// replacing any previously configured ALPNs.
    ///
    /// Defaults to \c "quic".  Use \c "h3" to interoperate with standard
    /// HTTP/3 clients and servers (e.g. \c curl \c --http3, ngtcp2 examples).
    void set_alpn(std::string alpn);

    /// Add an additional ALPN token to the list advertised during the TLS
    /// 1.3 handshake.  The peer selects one token from the list; the chosen
    /// token is available via \c connection::negotiated_alpn() after the
    /// handshake completes.
    ///
    /// Example — advertise both hq-interop and h3:
    /// \code
    ///   creds->set_alpn("hq-interop");
    ///   creds->add_alpn("h3");
    /// \endcode
    void add_alpn(std::string alpn);

private:
    std::unique_ptr<credentials_impl> _impl;
    friend class server;
    friend class connection;
    friend future<connection> connect(socket_address addr,
                                      shared_ptr<credentials> creds,
                                      connection_config config);
};

/// A single bidirectional QUIC stream within a connection.
///
/// Each stream provides independent `input_stream<char>` and
/// `output_stream<char>` interfaces, similar to a TCP
/// `connected_socket`, but multiplexed over a single QUIC connection.
class stream {
public:
    stream(stream&&) noexcept;
    stream& operator=(stream&&) noexcept;
    ~stream();

    /// The QUIC stream identifier.
    int64_t id() const noexcept;
    /// The read side of the stream.
    input_stream<char> input();
    /// The write side of the stream.
    output_stream<char> output(size_t buffer_size = 8192);
    /// Close the stream (sends FIN).
    future<> close();
private:
    explicit stream(lw_shared_ptr<stream_impl> p);
    lw_shared_ptr<stream_impl> _impl;
    friend class connection;
    friend connection_impl;
};

/// A QUIC connection (multiple multiplexed streams over a single UDP
/// 4-tuple).
///
/// Use `server::accept()` to obtain server-side connections, or
/// `connect()` to create client-side connections.
class connection {
public:
    connection(connection&&) noexcept;
    connection& operator=(connection&&) noexcept;
    ~connection();

    /// Open a new bidirectional stream on this connection.
    future<stream> open_stream();

    /// Open a new unidirectional stream on this connection.
    /// The returned stream can only be used for writing (output()).
    future<stream> open_uni_stream();

    /// Accept a remotely-initiated stream (bidirectional or unidirectional).
    future<stream> accept_stream();

    /// Write data directly to a stream identified by its QUIC stream ID.
    ///
    /// This bypasses the per-stream output_stream buffering and writes
    /// directly through ngtcp2.  Used by the HTTP/3 framing layer which
    /// manages stream multiplexing internally via nghttp3.
    ///
    /// \param stream_id The QUIC stream identifier.
    /// \param data Pointer to the data to send (may be nullptr if \p len is 0).
    /// \param len Number of bytes to send.
    /// \param fin If true, send a FIN after the data (closes the write side).
    future<> write_stream_data(int64_t stream_id, const uint8_t* data,
                               size_t len, bool fin);

    /// The ALPN protocol token selected during the TLS 1.3 handshake.
    ///
    /// Valid after the handshake completes (i.e. after \c server::accept()
    /// or \c connect() returns).  Empty if no ALPN was negotiated.
    std::string_view negotiated_alpn() const noexcept;

    /// The remote peer's address.
    socket_address remote_address() const noexcept;

    /// The local address.
    socket_address local_address() const noexcept;

    /// Gracefully close the connection.
    future<> close();

private:
    explicit connection(lw_shared_ptr<connection_impl> p);
    lw_shared_ptr<connection_impl> _impl;
    friend class server;
    friend future<connection> connect(socket_address addr,
                                      shared_ptr<credentials> creds,
                                      connection_config config);
};

/// A QUIC server that listens on a UDP socket and accepts incoming
/// connections.
///
/// Each shard should create its own server on the same address (the
/// kernel distributes incoming UDP packets via `SO_REUSEPORT`).
class server {
public:
    server(server&&) noexcept;
    server& operator=(server&&) noexcept;
    ~server();

    /// Start listening for QUIC connections.
    ///
    /// \param addr Local address to bind.
    /// \param creds TLS credentials (must have cert + key set).
    /// \param config Connection configuration defaults.
    static future<server> listen(socket_address addr,
                                 shared_ptr<credentials> creds,
                                 connection_config config = {});

    /// Accept the next incoming QUIC connection.
    future<connection> accept();

    /// Stop accepting new connections and close the listening socket.
    future<> close();

    /// The address this server is bound to.
    socket_address local_address() const noexcept;

private:
    class impl;
    explicit server(std::unique_ptr<impl> p);
    std::unique_ptr<impl> _impl;
};

/// Establish a client QUIC connection to the given address.
///
/// \param addr The remote address to connect to.
/// \param creds TLS credentials (should have trust CA set, or
///              set_no_verify() for testing).
/// \param config Connection configuration.
/// \returns A future that resolves when the QUIC handshake completes.
future<connection> connect(socket_address addr,
                           shared_ptr<credentials> creds,
                           connection_config config = {});

/// @}

} // namespace seastar::experimental::quic
