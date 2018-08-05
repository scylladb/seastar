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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#pragma once

#include <memory>
#include <vector>
#include <cstring>
#include "core/future.hh"
#include "net/byteorder.hh"
#include "net/socket_defs.hh"
#include "net/packet.hh"
#include "core/print.hh"
#include "core/temporary_buffer.hh"
#include "core/iostream.hh"
#include "util/std-compat.hh"
#include <sys/types.h>

namespace seastar {

static inline
bool is_ip_unspecified(ipv4_addr &addr) {
    return addr.ip == 0;
}

static inline
bool is_port_unspecified(ipv4_addr &addr) {
    return addr.port == 0;
}

static inline
std::ostream& operator<<(std::ostream &os, ipv4_addr addr) {
    fprint(os, "%d.%d.%d.%d",
            (addr.ip >> 24) & 0xff,
            (addr.ip >> 16) & 0xff,
            (addr.ip >> 8) & 0xff,
            (addr.ip) & 0xff);
    return os << ":" << addr.port;
}

static inline
socket_address make_ipv4_address(ipv4_addr addr) {
    socket_address sa;
    sa.u.in.sin_family = AF_INET;
    sa.u.in.sin_port = htons(addr.port);
    sa.u.in.sin_addr.s_addr = htonl(addr.ip);
    return sa;
}

inline
socket_address make_ipv4_address(uint32_t ip, uint16_t port) {
    socket_address sa;
    sa.u.in.sin_family = AF_INET;
    sa.u.in.sin_port = htons(port);
    sa.u.in.sin_addr.s_addr = htonl(ip);
    return sa;
}

namespace net {

// see linux tcp(7) for parameter explanation
struct tcp_keepalive_params {
    std::chrono::seconds idle; // TCP_KEEPIDLE
    std::chrono::seconds interval; // TCP_KEEPINTVL
    unsigned count; // TCP_KEEPCNT
};

// see linux sctp(7) for parameter explanation
struct sctp_keepalive_params {
    std::chrono::seconds interval; // spp_hbinterval
    unsigned count; // spp_pathmaxrt
};

using keepalive_params = compat::variant<tcp_keepalive_params, sctp_keepalive_params>;

/// \cond internal
class connected_socket_impl;
class socket_impl;
class server_socket_impl;
class udp_channel_impl;
class get_impl;
/// \endcond

class udp_datagram_impl {
public:
    virtual ~udp_datagram_impl() {};
    virtual ipv4_addr get_src() = 0;
    virtual ipv4_addr get_dst() = 0;
    virtual uint16_t get_dst_port() = 0;
    virtual packet& get_data() = 0;
};

class udp_datagram final {
private:
    std::unique_ptr<udp_datagram_impl> _impl;
public:
    udp_datagram(std::unique_ptr<udp_datagram_impl>&& impl) : _impl(std::move(impl)) {};
    ipv4_addr get_src() { return _impl->get_src(); }
    ipv4_addr get_dst() { return _impl->get_dst(); }
    uint16_t get_dst_port() { return _impl->get_dst_port(); }
    packet& get_data() { return _impl->get_data(); }
};

class udp_channel {
private:
    std::unique_ptr<udp_channel_impl> _impl;
public:
    udp_channel();
    udp_channel(std::unique_ptr<udp_channel_impl>);
    ~udp_channel();

    udp_channel(udp_channel&&);
    udp_channel& operator=(udp_channel&&);

    future<udp_datagram> receive();
    future<> send(ipv4_addr dst, const char* msg);
    future<> send(ipv4_addr dst, packet p);
    bool is_closed() const;
    void close();
};

} /* namespace net */

/// \addtogroup networking-module
/// @{

/// A TCP (or other stream-based protocol) connection.
///
/// A \c connected_socket represents a full-duplex stream between
/// two endpoints, a local endpoint and a remote endpoint.
class connected_socket {
    friend class net::get_impl;
    std::unique_ptr<net::connected_socket_impl> _csi;
public:
    /// Constructs a \c connected_socket not corresponding to a connection
    connected_socket();
    ~connected_socket();

    /// \cond internal
    explicit connected_socket(std::unique_ptr<net::connected_socket_impl> csi);
    /// \endcond
    /// Moves a \c connected_socket object.
    connected_socket(connected_socket&& cs) noexcept;
    /// Move-assigns a \c connected_socket object.
    connected_socket& operator=(connected_socket&& cs) noexcept;
    /// Gets the input stream.
    ///
    /// Gets an object returning data sent from the remote endpoint.
    input_stream<char> input();
    /// Gets the output stream.
    ///
    /// Gets an object that sends data to the remote endpoint.
    /// \param buffer_size how much data to buffer
    output_stream<char> output(size_t buffer_size = 8192);
    /// Sets the TCP_NODELAY option (disabling Nagle's algorithm)
    void set_nodelay(bool nodelay);
    /// Gets the TCP_NODELAY option (Nagle's algorithm)
    ///
    /// \return whether the nodelay option is enabled or not
    bool get_nodelay() const;
    /// Sets SO_KEEPALIVE option (enable keepalive timer on a socket)
    void set_keepalive(bool keepalive);
    /// Gets O_KEEPALIVE option
    /// \return whether the keepalive option is enabled or not
    bool get_keepalive() const;
    /// Sets TCP keepalive parameters
    void set_keepalive_parameters(const net::keepalive_params& p);
    /// Get TCP keepalive parameters
    net::keepalive_params get_keepalive_parameters() const;

    /// Disables output to the socket.
    ///
    /// Current or future writes that have not been successfully flushed
    /// will immediately fail with an error.  This is useful to abort
    /// operations on a socket that is not making progress due to a
    /// peer failure.
    void shutdown_output();
    /// Disables input from the socket.
    ///
    /// Current or future reads will immediately fail with an error.
    /// This is useful to abort operations on a socket that is not making
    /// progress due to a peer failure.
    void shutdown_input();
};
/// @}

/// \addtogroup networking-module
/// @{

/// The seastar socket.
///
/// A \c socket that allows a connection to be established between
/// two endpoints.
class socket {
    std::unique_ptr<net::socket_impl> _si;
public:
    ~socket();

    /// \cond internal
    explicit socket(std::unique_ptr<net::socket_impl> si);
    /// \endcond
    /// Moves a \c seastar::socket object.
    socket(socket&&) noexcept;
    /// Move-assigns a \c seastar::socket object.
    socket& operator=(socket&&) noexcept;

    /// Attempts to establish the connection.
    ///
    /// \return a \ref connected_socket representing the connection.
    future<connected_socket> connect(socket_address sa, socket_address local = socket_address(::sockaddr_in{AF_INET, INADDR_ANY, {0}}), transport proto = transport::TCP);
    /// Stops any in-flight connection attempt.
    ///
    /// Cancels the connection attempt if it's still in progress, and
    /// terminates the connection if it has already been established.
    void shutdown();
};

/// @}

/// \addtogroup networking-module
/// @{

/// A listening socket, waiting to accept incoming network connections.
class server_socket {
    std::unique_ptr<net::server_socket_impl> _ssi;
    bool _aborted = false;
public:
    enum class load_balancing_algorithm {
        // This algorithm tries to distribute all connections equally between all shards.
        // It does this by sending new connections to a shard with smallest amount of connections.
        connection_distribution,
        // This algorithm distributes new connection based on peer's tcp port. Destination shard
        // is calculated as a port number modulo number of shards. This allows a client to connect
        // to a specific shard in a server given it knows how many shards server has by choosing
        // src port number accordingly.
        port,
        default_ = connection_distribution
    };
    /// Constructs a \c server_socket not corresponding to a connection
    server_socket();
    /// \cond internal
    explicit server_socket(std::unique_ptr<net::server_socket_impl> ssi);
    /// \endcond
    /// Moves a \c server_socket object.
    server_socket(server_socket&& ss) noexcept;
    ~server_socket();
    /// Move-assigns a \c server_socket object.
    server_socket& operator=(server_socket&& cs) noexcept;

    /// Accepts the next connection to successfully connect to this socket.
    ///
    /// \return a \ref connected_socket representing the connection, and
    ///         a \ref socket_address describing the remote endpoint.
    ///
    /// \see listen(socket_address sa)
    /// \see listen(socket_address sa, listen_options opts)
    future<connected_socket, socket_address> accept();

    /// Stops any \ref accept() in progress.
    ///
    /// Current and future \ref accept() calls will terminate immediately
    /// with an error.
    void abort_accept();
};
/// @}

struct listen_options {
    bool reuse_address = false;
    server_socket::load_balancing_algorithm lba = server_socket::load_balancing_algorithm::default_;
    transport proto = transport::TCP;
};

class network_stack {
public:
    virtual ~network_stack() {}
    virtual server_socket listen(socket_address sa, listen_options opts) = 0;
    // FIXME: local parameter assumes ipv4 for now, fix when adding other AF
    future<connected_socket> connect(socket_address sa, socket_address local = socket_address(::sockaddr_in{AF_INET, INADDR_ANY, {0}}), transport proto = transport::TCP) {
        return socket().connect(sa, local, proto);
    }
    virtual ::seastar::socket socket() = 0;
    virtual net::udp_channel make_udp_channel(ipv4_addr addr = {}) = 0;
    virtual future<> initialize() {
        return make_ready_future();
    }
    virtual bool has_per_core_namespace() = 0;
};

}
