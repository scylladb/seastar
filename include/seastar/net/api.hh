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

#ifndef SEASTAR_MODULE
#include <chrono>
#include <memory>
#include <vector>
#include <cstring>
#include <sys/types.h>
#endif

#include <seastar/core/future.hh>
#include <seastar/net/byteorder.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/net/packet.hh>
#include <seastar/core/internal/api-level.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/file-types.hh>
#include <seastar/core/iostream.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/program-options.hh>
#include <seastar/util/modules.hh>

namespace seastar {

inline
bool is_ip_unspecified(const ipv4_addr& addr) noexcept {
    return addr.is_ip_unspecified();
}

inline
bool is_port_unspecified(const ipv4_addr& addr) noexcept {
    return addr.is_port_unspecified();
}

inline
socket_address make_ipv4_address(const ipv4_addr& addr) noexcept {
    return socket_address(addr);
}

inline
socket_address make_ipv4_address(uint32_t ip, uint16_t port) noexcept {
    return make_ipv4_address(ipv4_addr(ip, port));
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

using keepalive_params = std::variant<tcp_keepalive_params, sctp_keepalive_params>;

/// \cond internal
class connected_socket_impl;
class socket_impl;

class server_socket_impl;
class datagram_channel_impl;
class get_impl;
/// \endcond

class datagram_impl {
public:
    virtual ~datagram_impl() {};
    virtual socket_address get_src() = 0;
    virtual socket_address get_dst() = 0;
    virtual uint16_t get_dst_port() = 0;
    virtual packet& get_data() = 0;
};

using udp_datagram_impl = datagram_impl;

class datagram final {
private:
    std::unique_ptr<datagram_impl> _impl;
public:
    datagram(std::unique_ptr<datagram_impl>&& impl) noexcept : _impl(std::move(impl)) {};
    socket_address get_src() { return _impl->get_src(); }
    socket_address get_dst() { return _impl->get_dst(); }
    uint16_t get_dst_port() { return _impl->get_dst_port(); }
    packet& get_data() { return _impl->get_data(); }
};

using udp_datagram = datagram;

class datagram_channel {
private:
    std::unique_ptr<datagram_channel_impl> _impl;
public:
    datagram_channel() noexcept;
    datagram_channel(std::unique_ptr<datagram_channel_impl>) noexcept;
    ~datagram_channel();

    datagram_channel(datagram_channel&&) noexcept;
    datagram_channel& operator=(datagram_channel&&) noexcept;

    socket_address local_address() const;

    future<datagram> receive();
    future<> send(const socket_address& dst, const char* msg);
    future<> send(const socket_address& dst, packet p);
    bool is_closed() const;
    /// Causes a pending receive() to complete (possibly with an exception)
    void shutdown_input();
    /// Causes a pending send() to complete (possibly with an exception)
    void shutdown_output();
    /// Close the channel and releases all resources.
    ///
    /// Must be called only when there are no unfinished send() or receive() calls. You
    /// can force pending calls to complete soon by calling shutdown_input() and
    /// shutdown_output().
    void close();
};

using udp_channel = datagram_channel;

class network_interface_impl;

} /* namespace net */

/// \addtogroup networking-module
/// @{

/// Configuration for buffered connected_socket input operations
///
/// This structure allows tuning of buffered input operations done via
/// connected_socket. It is a hint to the implementation and may be
/// ignored (e.g. the zero-copy native stack does not allocate buffers,
/// so it ignores buffer-size parameters).
struct connected_socket_input_stream_config final {
    /// Initial buffer size to use for input buffering
    unsigned buffer_size = 8192;
    /// Minimum buffer size to use for input buffering. The system will decrease
    /// buffer sizes if it sees a tendency towards small requests, but will not go
    /// below this buffer size.
    unsigned min_buffer_size = 512;
    /// Maximum buffer size to use for input buffering. The system will increase
    /// buffer sizes if it sees a tendency towards large requests, but will not go
    /// above this buffer size.
    unsigned max_buffer_size = 128 * 1024;
};

/// Distinguished name
struct session_dn {
    sstring subject;
    sstring issuer;
};

/// A TCP (or other stream-based protocol) connection.
///
/// A \c connected_socket represents a full-duplex stream between
/// two endpoints, a local endpoint and a remote endpoint.
class connected_socket {
    friend class net::get_impl;
    std::unique_ptr<net::connected_socket_impl> _csi;
public:
    /// Constructs a \c connected_socket not corresponding to a connection
    connected_socket() noexcept;
    ~connected_socket();

    /// \cond internal
    explicit connected_socket(std::unique_ptr<net::connected_socket_impl> csi) noexcept;
    /// \endcond
    /// Moves a \c connected_socket object.
    connected_socket(connected_socket&& cs) noexcept;
    /// Move-assigns a \c connected_socket object.
    connected_socket& operator=(connected_socket&& cs) noexcept;
    /// Gets the input stream.
    ///
    /// \param csisc Configuration for the input_stream returned
    ///
    /// Gets an object returning data sent from the remote endpoint.
    input_stream<char> input(connected_socket_input_stream_config csisc = {});
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
    /// Sets custom socket options. Based on setsockopt function.
    /// Linux users should refer to protocol-specific manuals
    /// to see available options, e.g. tcp(7), ip(7), etc.
    void set_sockopt(int level, int optname, const void* data, size_t len);
    /// Gets custom socket options. Based on getsockopt function.
    /// Linux users should refer to protocol-specific manuals
    /// to see available options, e.g. tcp(7), ip(7), etc.
    int get_sockopt(int level, int optname, void* data, size_t len) const;
    /// Local address of the socket
    socket_address local_address() const noexcept;
    /// Remote address of the socket
    socket_address remote_address() const noexcept;

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
    /// Check whether the \c connected_socket is initialized.
    ///
    /// \return true if this \c connected_socket socket_address is bound initialized
    /// false otherwise.
    ///
    /// \see connect(socket_address sa)
    /// \see connect(socket_address sa, socket_address local, transport proto)
    explicit operator bool() const noexcept {
        return static_cast<bool>(_csi);
    }
    /// Waits for the peer of this socket to disconnect
    ///
    /// \return future that resolves when the peer closes connection or shuts it down
    /// for writing or when local socket is called \ref shutdown_input().
    ///
    /// Note, that when the returned future is resolved for whatever reason socket
    /// may still be readable from, so the caller may want to wait for both events
    /// -- this one and EOF from read.
    ///
    /// Calling it several times per socket is not allowed (undefined behavior)
    ///
    /// \see poll(2) about POLLRDHUP for more details
    future<> wait_input_shutdown();
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
    socket() noexcept = default;
    ~socket();

    /// \cond internal
    explicit socket(std::unique_ptr<net::socket_impl> si) noexcept;
    /// \endcond
    /// Moves a \c seastar::socket object.
    socket(socket&&) noexcept;
    /// Move-assigns a \c seastar::socket object.
    socket& operator=(socket&&) noexcept;

    /// Attempts to establish the connection.
    ///
    /// \return a \ref connected_socket representing the connection.
    future<connected_socket> connect(socket_address sa, socket_address local = {}, transport proto = transport::TCP);

    /// Sets SO_REUSEADDR option (enable reuseaddr option on a socket)
    void set_reuseaddr(bool reuseaddr);
    /// Gets O_REUSEADDR option
    /// \return whether the reuseaddr option is enabled or not
    bool get_reuseaddr() const;
    /// Stops any in-flight connection attempt.
    ///
    /// Cancels the connection attempt if it's still in progress, and
    /// terminates the connection if it has already been established.
    void shutdown();
};

/// @}

/// \addtogroup networking-module
/// @{

/// The result of an server_socket::accept() call
struct accept_result {
    connected_socket connection;  ///< The newly-accepted connection
    socket_address remote_address;  ///< The address of the peer that connected to us
};

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
        // This algorithm distributes all new connections to listen_options::fixed_cpu shard only.
        fixed,
        default_ = connection_distribution
    };
    /// Constructs a \c server_socket without being bound to any address
    server_socket() noexcept;
    /// \cond internal
    explicit server_socket(std::unique_ptr<net::server_socket_impl> ssi) noexcept;
    /// \endcond
    /// Moves a \c server_socket object.
    server_socket(server_socket&& ss) noexcept;
    ~server_socket();
    /// Move-assigns a \c server_socket object.
    server_socket& operator=(server_socket&& cs) noexcept;

    /// Accepts the next connection to successfully connect to this socket.
    ///
    /// \return an accept_result representing the connection and
    ///         the socket_address of the remote endpoint.
    ///
    /// \see listen(socket_address sa)
    /// \see listen(socket_address sa, listen_options opts)
    future<accept_result> accept();

    /// Stops any \ref accept() in progress.
    ///
    /// Current and future \ref accept() calls will terminate immediately
    /// with an error.
    void abort_accept();

    /// Local bound address
    ///
    /// \return the local bound address if the \c server_socket is listening,
    /// an empty address constructed with \c socket_address() otherwise.
    ///
    /// \see listen(socket_address sa)
    /// \see listen(socket_address sa, listen_options opts)
    socket_address local_address() const noexcept;

    /// Check whether the \c server_socket is listening on any address.
    ///
    /// \return true if this \c socket_address is bound to an address,
    /// false if it is just created with the default constructor.
    ///
    /// \see listen(socket_address sa)
    /// \see listen(socket_address sa, listen_options opts)
    explicit operator bool() const noexcept {
        return static_cast<bool>(_ssi);
    }
};

/// @}

/// Options for creating a listening socket.
///
/// WARNING: these options currently only have an effect when using
/// the POSIX stack: all options are ignored on the native stack as they
/// are not implemented there.
struct listen_options {
    bool reuse_address = false;
    server_socket::load_balancing_algorithm lba = server_socket::load_balancing_algorithm::default_;
    transport proto = transport::TCP;
    int listen_backlog = 100;
    unsigned fixed_cpu = 0u;
    std::optional<file_permissions> unix_domain_socket_permissions;

    /// If set, the SO_SNDBUF size will be set to the given value on the listening socket
    /// via setsockopt. This buffer size is inherited by the sockets returned by
    /// accept and is the preferred way to set the buffer size for these sockets since
    /// setting it directly on the already-accepted socket is ineffective (see TCP(7)).
    std::optional<int> so_sndbuf;

    /// If set, the SO_RCVBUF size will be set to the given value on the listening socket
    /// via setsockopt. This buffer size is inherited by the sockets returned by
    /// accept and is the preferred way to set the buffer size for these sockets since
    /// setting it directly on the already-accepted socket is ineffective (see TCP(7)).
    std::optional<int> so_rcvbuf;

    void set_fixed_cpu(unsigned cpu) {
        lba = server_socket::load_balancing_algorithm::fixed;
        fixed_cpu = cpu;
    }
};

class network_interface {
private:
    shared_ptr<net::network_interface_impl> _impl;
public:
    network_interface() = delete;
    network_interface(shared_ptr<net::network_interface_impl>) noexcept;
    network_interface(network_interface&&) noexcept;

    network_interface& operator=(network_interface&&) noexcept;

    uint32_t index() const;
    uint32_t mtu() const;

    const sstring& name() const;
    const sstring& display_name() const;
    const std::vector<net::inet_address>& addresses() const;
    const std::vector<uint8_t> hardware_address() const;

    bool is_loopback() const;
    bool is_virtual() const;
    bool is_up() const;
    bool supports_ipv6() const;
};

struct statistics {
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
};

namespace metrics {
class metric_groups;
class label_instance;
}

void register_net_metrics_for_scheduling_group(
    metrics::metric_groups& m, unsigned sg_id, const metrics::label_instance& name);

class network_stack {
public:
    virtual ~network_stack() {}
    virtual server_socket listen(socket_address sa, listen_options opts) = 0;
    // FIXME: local parameter assumes ipv4 for now, fix when adding other AF
    future<connected_socket> connect(socket_address sa, socket_address = {}, transport proto = transport::TCP);
    virtual ::seastar::socket socket() = 0;

    [[deprecated("Use `make_[un]bound_datagram_channel` instead")]]
    virtual net::udp_channel make_udp_channel(const socket_address& = {}) = 0;

    virtual net::datagram_channel make_unbound_datagram_channel(sa_family_t) = 0;
    virtual net::datagram_channel make_bound_datagram_channel(const socket_address& local) = 0;
    virtual future<> initialize() {
        return make_ready_future();
    }
    virtual bool has_per_core_namespace() = 0;
    // NOTE: this is not a correct query approach.
    // This question should be per NIC, but we have no such
    // abstraction, so for now this is "stack-wide"
    virtual bool supports_ipv6() const {
        return false;
    }

    // Return network stats (bytes sent/received etc.) for this stack and scheduling group
    virtual statistics stats(unsigned scheduling_group_id) = 0;
    // Clears the stats for this stack and scheduling group
    virtual void clear_stats(unsigned scheduling_group_id) = 0;

    /**
     * Returns available network interfaces. This represents a
     * snapshot of interfaces available at call time, hence the
     * return by value.
     */
    virtual std::vector<network_interface> network_interfaces();
};

struct network_stack_entry {
    using factory_func = noncopyable_function<future<std::unique_ptr<network_stack>> (const program_options::option_group&)>;

    sstring name;
    std::unique_ptr<program_options::option_group> opts;
    factory_func factory;
    bool is_default;
};

}
