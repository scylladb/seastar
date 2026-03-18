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

#ifdef SEASTAR_MODULE
module;
#endif

#include <chrono>
#include <cstring>
#include <functional>
#include <random>
#include <variant>
#include <coroutine>

#include <unistd.h>
#include <linux/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <arpa/inet.h>
#include <net/route.h>
#include <netinet/tcp.h>
#include <netinet/sctp.h>
#include <sys/socket.h>
#include <seastar/util/assert.hh>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/loop.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/net/posix-stack.hh>
#include <seastar/net/net.hh>
#include <seastar/net/packet.hh>
#include <seastar/net/api.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/util/std-compat.hh>
#endif

namespace std {

template <>
struct hash<seastar::net::posix_ap_server_socket_impl::protocol_and_socket_address> {
    size_t operator()(const seastar::net::posix_ap_server_socket_impl::protocol_and_socket_address& t_sa) const {
        auto h1 = std::hash<int>()(std::get<0>(t_sa));
        auto h2 = std::hash<seastar::net::socket_address>()(std::get<1>(t_sa));
        return h1 ^ h2;
    }
};

}


namespace {

// reinterpret_cast<foo*>() on a pointer that the compiler knows points to an
// object with a different type is disliked by the compiler as it violates
// strict aliasing rules. This safe version does the same thing but keeps the
// compiler happy.
template <typename T>
T
copy_reinterpret_cast(const void* ptr) {
    T tmp;
    std::memcpy(&tmp, ptr, sizeof(T));
    return tmp;
}

thread_local std::array<uint64_t, seastar::max_scheduling_groups()> bytes_sent = {};
thread_local std::array<uint64_t, seastar::max_scheduling_groups()> bytes_received = {};

}

namespace seastar {

namespace internal {

std::pair<size_t, deleter> wrapped_iovecs::populate(std::span<temporary_buffer<char>> bufs) {
    size_t total = 0;
    deleter del;

    if (!v.empty()) {
        on_internal_error(seastar_logger, "wrapped_iovecs are re-populated live");
    }

    v.reserve(bufs.size());
    for (auto&& buf : bufs) {
        auto b = std::move(buf);
        total += b.size();
        v.push_back({ .iov_base = b.get_write(), .iov_len = b.size() });
        deleter d = b.release();
        d.append(std::move(del));
        del = std::move(d);
    }

    return std::make_pair(total, make_deleter(std::move(del), [this] { v.clear(); }));
}

}

namespace net {

using namespace seastar;

class posix_connected_socket_operations {
public:
    virtual ~posix_connected_socket_operations() = default;
    virtual void set_nodelay(file_desc& fd, bool nodelay) const = 0;
    virtual bool get_nodelay(file_desc& fd) const = 0;
    virtual void set_keepalive(file_desc& _fd, bool keepalive) const = 0;
    virtual bool get_keepalive(file_desc& _fd) const = 0;
    virtual void set_keepalive_parameters(file_desc& _fd, const keepalive_params& params) const = 0;
    virtual keepalive_params get_keepalive_parameters(file_desc& _fd) const = 0;
    virtual void set_sockopt(file_desc& _fd, int level, int optname, const void* data, size_t len) const {
        _fd.setsockopt(level, optname, data, socklen_t(len));
    }
    virtual int get_sockopt(file_desc& _fd, int level, int optname, void* data, size_t len) const {
        return _fd.getsockopt(level, optname, reinterpret_cast<char*>(data), socklen_t(len));
    }
    virtual socket_address local_address(file_desc& _fd) const {
        return _fd.get_address();
    }
    virtual socket_address remote_address(file_desc& _fd) const {
        try {
            return _fd.get_remote_address();
        } catch (const std::system_error& e) {
            if (e.code().value() != ENOTCONN) {
                throw;
            }
            return socket_address();
        }
    }
};

thread_local posix_ap_server_socket_impl::port_map_t posix_ap_server_socket_impl::ports{};
thread_local posix_ap_server_socket_impl::sockets_map_t posix_ap_server_socket_impl::sockets{};
thread_local posix_ap_server_socket_impl::conn_map_t posix_ap_server_socket_impl::conn_q{};

class posix_tcp_connected_socket_operations : public posix_connected_socket_operations {
public:
    virtual void set_nodelay(file_desc& _fd, bool nodelay) const override {
        _fd.setsockopt(IPPROTO_TCP, TCP_NODELAY, int(nodelay));
    }
    virtual bool get_nodelay(file_desc& _fd) const override {
        return _fd.getsockopt<int>(IPPROTO_TCP, TCP_NODELAY);
    }
    virtual void set_keepalive(file_desc& _fd, bool keepalive) const override {
        _fd.setsockopt(SOL_SOCKET, SO_KEEPALIVE, int(keepalive));
    }
    virtual bool get_keepalive(file_desc& _fd) const override {
        return _fd.getsockopt<int>(SOL_SOCKET, SO_KEEPALIVE);
    }
    virtual void set_keepalive_parameters(file_desc& _fd, const keepalive_params& params) const override {
        const tcp_keepalive_params& pms = std::get<tcp_keepalive_params>(params);
        _fd.setsockopt(IPPROTO_TCP, TCP_KEEPCNT, pms.count);
        _fd.setsockopt(IPPROTO_TCP, TCP_KEEPIDLE, int(pms.idle.count()));
        _fd.setsockopt(IPPROTO_TCP, TCP_KEEPINTVL, int(pms.interval.count()));
    }
    virtual keepalive_params get_keepalive_parameters(file_desc& _fd) const override {
        return tcp_keepalive_params {
            std::chrono::seconds(_fd.getsockopt<int>(IPPROTO_TCP, TCP_KEEPIDLE)),
            std::chrono::seconds(_fd.getsockopt<int>(IPPROTO_TCP, TCP_KEEPINTVL)),
            _fd.getsockopt<unsigned>(IPPROTO_TCP, TCP_KEEPCNT)
        };
    }
};

class posix_sctp_connected_socket_operations : public posix_connected_socket_operations {
public:
    virtual void set_nodelay(file_desc& _fd, bool nodelay) const override {
        _fd.setsockopt(SOL_SCTP, SCTP_NODELAY, int(nodelay));
    }
    virtual bool get_nodelay(file_desc& _fd) const override {
        return _fd.getsockopt<int>(SOL_SCTP, SCTP_NODELAY);
    }
    virtual void set_keepalive(file_desc& _fd, bool keepalive) const override {
        auto heartbeat = _fd.getsockopt<sctp_paddrparams>(SOL_SCTP, SCTP_PEER_ADDR_PARAMS);
        if (keepalive) {
            heartbeat.spp_flags |= SPP_HB_ENABLE;
        } else {
            heartbeat.spp_flags &= ~SPP_HB_ENABLE;
        }
        _fd.setsockopt(SOL_SCTP, SCTP_PEER_ADDR_PARAMS, heartbeat);
    }
    virtual bool get_keepalive(file_desc& _fd) const override {
        return _fd.getsockopt<sctp_paddrparams>(SOL_SCTP, SCTP_PEER_ADDR_PARAMS).spp_flags & SPP_HB_ENABLE;
    }
    virtual void set_keepalive_parameters(file_desc& _fd, const keepalive_params& kpms) const override {
        const sctp_keepalive_params& pms = std::get<sctp_keepalive_params>(kpms);
        auto params = _fd.getsockopt<sctp_paddrparams>(SOL_SCTP, SCTP_PEER_ADDR_PARAMS);
        params.spp_hbinterval = pms.interval.count() * 1000; // in milliseconds
        params.spp_pathmaxrxt = pms.count;
        _fd.setsockopt(SOL_SCTP, SCTP_PEER_ADDR_PARAMS, params);
    }
    virtual keepalive_params get_keepalive_parameters(file_desc& _fd) const override {
        auto params = _fd.getsockopt<sctp_paddrparams>(SOL_SCTP, SCTP_PEER_ADDR_PARAMS);
        return sctp_keepalive_params {
            std::chrono::seconds(params.spp_hbinterval/1000), // in seconds
            params.spp_pathmaxrxt
        };
    }
};

class posix_unix_stream_connected_socket_operations : public posix_connected_socket_operations {
public:
    virtual void set_nodelay(file_desc& fd, bool nodelay) const override {
        SEASTAR_ASSERT(nodelay); // make sure nobody actually tries to use this non-existing functionality
    }
    virtual bool get_nodelay(file_desc& fd) const override {
        return true;
    }
    virtual void set_keepalive(file_desc& fd, bool keepalive) const override {}
    virtual bool get_keepalive(file_desc& fd) const override {
        return false;
    }
    virtual void set_keepalive_parameters(file_desc& fd, const keepalive_params& p) const override {}
    virtual keepalive_params get_keepalive_parameters(file_desc& fd) const override {
        return keepalive_params{};
    }
};

static const posix_connected_socket_operations*
get_posix_connected_socket_ops(sa_family_t family, int protocol) {
    static posix_tcp_connected_socket_operations tcp_ops;
    static posix_sctp_connected_socket_operations sctp_ops;
    static posix_unix_stream_connected_socket_operations unix_ops;
    switch (family) {
    case AF_INET:
    case AF_INET6:
        switch (protocol) {
        case IPPROTO_TCP: return &tcp_ops;
        case IPPROTO_SCTP: return &sctp_ops;
        default: abort();
        }
    case AF_UNIX:
        return &unix_ops;
    default:
        abort();
    }
}

static void shutdown_socket_fd(pollable_fd& fd, int how) noexcept {
    try {
        // file_desc::shutdown ignores ENOTCONN. Other reasons for exception
        // EINVAL (wrong "how") -- impossible
        // ENOTSOCK (not a socket) -- incredible
        // EBADF (invalid file descriptor) -- irretrievable
        fd.shutdown(how);
    } catch (...) {
        on_internal_error(seastar_logger, seastar::format("socket shutdown({}, {}) failed: {}", fd.get_file_desc().fdinfo(), how, std::current_exception()));
    }
}

class posix_connected_socket_impl : public connected_socket_impl {
    pollable_fd _fd;
    const posix_connected_socket_operations* _ops;
    conntrack::handle _handle;
    std::pmr::polymorphic_allocator<char>* _allocator;
public:
    explicit posix_connected_socket_impl(sa_family_t family, int protocol, pollable_fd fd, std::pmr::polymorphic_allocator<char>* allocator=memory::malloc_allocator) :
        _fd(std::move(fd)), _ops(get_posix_connected_socket_ops(family, protocol)), _allocator(allocator) {}
    explicit posix_connected_socket_impl(sa_family_t family, int protocol, pollable_fd fd, conntrack::handle&& handle,
        std::pmr::polymorphic_allocator<char>* allocator=memory::malloc_allocator) : _fd(std::move(fd))
                , _ops(get_posix_connected_socket_ops(family, protocol)), _handle(std::move(handle)), _allocator(allocator) {}
public:
    virtual data_source source() override {
        return source(connected_socket_input_stream_config());
    }
    virtual data_source source(connected_socket_input_stream_config csisc) override {
        return data_source(std::make_unique<posix_data_source_impl>(_fd, csisc, _allocator));
    }
    virtual data_sink sink() override {
        return data_sink(std::make_unique< posix_data_sink_impl>(_fd));
    }
    virtual void shutdown_input() override {
        shutdown_socket_fd(_fd, SHUT_RD);
    }
    virtual void shutdown_output() override {
        shutdown_socket_fd(_fd, SHUT_WR);
    }
    virtual void set_nodelay(bool nodelay) override {
        return _ops->set_nodelay(_fd.get_file_desc(), nodelay);
    }
    virtual bool get_nodelay() const override {
        return _ops->get_nodelay(_fd.get_file_desc());
    }
    void set_keepalive(bool keepalive) override {
        return _ops->set_keepalive(_fd.get_file_desc(), keepalive);
    }
    bool get_keepalive() const override {
        return _ops->get_keepalive(_fd.get_file_desc());
    }
    void set_keepalive_parameters(const keepalive_params& p) override {
        return _ops->set_keepalive_parameters(_fd.get_file_desc(), p);
    }
    keepalive_params get_keepalive_parameters() const override {
        return _ops->get_keepalive_parameters(_fd.get_file_desc());
    }
    void set_sockopt(int level, int optname, const void* data, size_t len) override {
        return _ops->set_sockopt(_fd.get_file_desc(), level, optname, data, len);
    }
    int get_sockopt(int level, int optname, void* data, size_t len) const override {
        return _ops->get_sockopt(_fd.get_file_desc(), level, optname, data, len);
    }
    socket_address local_address() const noexcept override {
        return _ops->local_address(_fd.get_file_desc());
    }
    socket_address remote_address() const noexcept override {
        return _ops->remote_address(_fd.get_file_desc());
    }
    future<> wait_input_shutdown() override {
        return _fd.poll_rdhup();
    }

    friend class posix_server_socket_impl;
    friend class posix_ap_server_socket_impl;
    friend class posix_reuseport_server_socket_impl;
    friend class posix_network_stack;
    friend class posix_ap_network_stack;
    friend class posix_socket_impl;
};

// Like posix_connected_socket_impl, but overrides local_address() and remote_address()
// to return the addresses from the PROXY protocol v2 header.
class posix_proxied_connected_socket_impl final : public posix_connected_socket_impl {
    socket_address _proxy_local_addr;
    socket_address _proxy_remote_addr;
public:
    posix_proxied_connected_socket_impl(
            sa_family_t family,
            int protocol,
            pollable_fd fd,
            conntrack::handle&& handle,
            socket_address proxy_local_addr,
            socket_address proxy_remote_addr,
            std::pmr::polymorphic_allocator<char>* allocator = memory::malloc_allocator)
        : posix_connected_socket_impl(family, protocol, std::move(fd), std::move(handle), allocator)
        , _proxy_local_addr(proxy_local_addr)
        , _proxy_remote_addr(proxy_remote_addr) {
    }
    virtual socket_address local_address() const noexcept override {
        return _proxy_local_addr;
    }
    virtual socket_address remote_address() const noexcept override {
        return _proxy_remote_addr;
    }
};

static void resolve_outgoing_address(socket_address& a) {
    if (a.family() != AF_INET6
        || a.as_posix_sockaddr_in6().sin6_scope_id != inet_address::invalid_scope
        || !IN6_IS_ADDR_LINKLOCAL(&a.as_posix_sockaddr_in6().sin6_addr)
    ) {
        return;
    }

    FILE *f;

    if (!(f = fopen("/proc/net/ipv6_route", "r"))) {
        throw std::system_error(errno, std::system_category(), "resolve_address");
    }

    auto holder = std::unique_ptr<FILE, int(*)(FILE *)>(f, &::fclose);

    /**
      Here all configured IPv6 routes are shown in a special format. The example displays for loopback interface only. The meaning is shown below (see net/ipv6/route.c for more).

    # cat /proc/net/ipv6_route
    00000000000000000000000000000000 00 00000000000000000000000000000000 00 00000000000000000000000000000000 ffffffff 00000001 00000001 00200200 lo
    +------------------------------+ ++ +------------------------------+ ++ +------------------------------+ +------+ +------+ +------+ +------+ ++
    |                                |  |                                |  |                                |        |        |        |        |
    1                                2  3                                4  5                                6        7        8        9        10

    1: IPv6 destination network displayed in 32 hexadecimal chars without colons as separator

    2: IPv6 destination prefix length in hexadecimal

    3: IPv6 source network displayed in 32 hexadecimal chars without colons as separator

    4: IPv6 source prefix length in hexadecimal

    5: IPv6 next hop displayed in 32 hexadecimal chars without colons as separator

    6: Metric in hexadecimal

    7: Reference counter

    8: Use counter

    9: Flags

    10: Device name

    */

    uint32_t prefix_len, src_prefix_len;
    unsigned long flags;
    char device[16];
    char dest_str[40];

    for (;;) {
        auto n = fscanf(f, "%4s%4s%4s%4s%4s%4s%4s%4s %02x "
                            "%*4s%*4s%*4s%*4s%*4s%*4s%*4s%*4s %02x "
                            "%*4s%*4s%*4s%*4s%*4s%*4s%*4s%*4s "
                            "%*08x %*08x %*08x %08lx %8s",
                            &dest_str[0], &dest_str[5], &dest_str[10], &dest_str[15],
                            &dest_str[20], &dest_str[25], &dest_str[30], &dest_str[35],
                            &prefix_len,
                            &src_prefix_len,
                            &flags, device);
        if (n != 12) {
            break;
        }

        if ((prefix_len > 128)  || (src_prefix_len != 0)
            || (flags & (RTF_POLICY | RTF_FLOW))
            || ((flags & RTF_REJECT) && prefix_len == 0) /* reject all */) {
            continue;
        }

        dest_str[4] = dest_str[9] = dest_str[14] = dest_str[19] = dest_str[24] = dest_str[29] = dest_str[34] = ':';
        dest_str[39] = '\0';

        struct in6_addr addr;
        if (inet_pton(AF_INET6, dest_str, &addr) < 0) {
            /* not an Ipv6 address */
            continue;
        }

        auto bytes = prefix_len / 8;
        auto bits = prefix_len % 8;

        auto& src = a.as_posix_sockaddr_in6().sin6_addr;

        if (bytes > 0 && memcmp(&src, &addr, bytes)) {
            continue;
        }
        if (bits > 0) {
            auto c1 = src.s6_addr[bytes];
            auto c2 = addr.s6_addr[bytes];
            auto mask = 0xffu << (8 - bits);
            if ((c1 & mask) != (c2 & mask)) {
                continue;
            }
        }

        // found the route.
        for (auto& nif : engine().net().network_interfaces()) {
            if (nif.name() == device || nif.display_name() == device) {
                a.as_posix_sockaddr_in6().sin6_scope_id = nif.index();
                return;
            }
        }
    }
}

class posix_socket_impl final : public socket_impl {
    pollable_fd _fd;
    std::pmr::polymorphic_allocator<char>* _allocator;
    bool _reuseaddr = false;

    future<> find_port_and_connect(socket_address sa, socket_address local, transport proto = transport::TCP) {
        static thread_local std::default_random_engine random_engine{std::random_device{}()};
        static thread_local std::uniform_int_distribution<uint16_t> u(49152/smp::count + 1, 65535/smp::count - 1);
        // If no explicit local address, set to dest address family wildcard.
        if (local.is_unspecified()) {
            local = net::inet_address(sa.addr().in_family());
        }
        resolve_outgoing_address(sa);
        return repeat([this, sa, local, proto, attempts = 0, requested_port = ntoh(local.as_posix_sockaddr_in().sin_port)] () mutable {
            _fd = engine().make_pollable_fd(sa, int(proto));
            _fd.get_file_desc().setsockopt(SOL_SOCKET, SO_REUSEADDR, int(_reuseaddr));
            uint16_t port = attempts++ < 5 && requested_port == 0 && proto == transport::TCP ? u(random_engine) * smp::count + this_shard_id() : requested_port;
            local.as_posix_sockaddr_in().sin_port = hton(port);
            return futurize_invoke([this, sa, local] { return internal::posix_connect(_fd, sa, local); }).then_wrapped([port, requested_port] (future<> f) {
                try {
                    f.get();
                    return stop_iteration::yes;
                } catch (std::system_error& err) {
                    if (port != requested_port && (err.code().value() == EADDRINUSE || err.code().value() == EADDRNOTAVAIL)) {
                        return stop_iteration::no;
                    }
                    throw;
                }
            });
        });
    }

    /// an aux function to handle unix-domain-specific requests
    future<connected_socket> connect_unix_domain(socket_address sa, socket_address local) {
        // note that if the 'local' address was not set by the client, it was created as an undefined address
        if (local.is_unspecified()) {
            local = socket_address{unix_domain_addr{std::string{}}};
        }

        _fd = engine().make_pollable_fd(sa, 0);
        return internal::posix_connect(_fd, sa, local).then(
            [fd = _fd, allocator = _allocator](){
                // a problem with 'private' interaction with 'unique_ptr'
                std::unique_ptr<connected_socket_impl> csi(new posix_connected_socket_impl{AF_UNIX, 0, std::move(fd), allocator});
                return make_ready_future<connected_socket>(connected_socket(std::move(csi)));
            }
        );
    }

public:
    explicit posix_socket_impl(std::pmr::polymorphic_allocator<char>* allocator=memory::malloc_allocator) : _allocator(allocator) {}

    virtual future<connected_socket> connect(socket_address sa, socket_address local, transport proto = transport::TCP) override {
        if (sa.is_af_unix()) {
            return connect_unix_domain(sa, local);
        }
        return find_port_and_connect(sa, local, proto).then([this, sa, proto, allocator = _allocator] () mutable {
            std::unique_ptr<connected_socket_impl> csi(new posix_connected_socket_impl(sa.family(), static_cast<int>(proto), _fd, allocator));
            return make_ready_future<connected_socket>(connected_socket(std::move(csi)));
        });
    }

    void set_reuseaddr(bool reuseaddr) override {
        _reuseaddr = reuseaddr;
        if (_fd) {
            _fd.get_file_desc().setsockopt(SOL_SOCKET, SO_REUSEADDR, int(reuseaddr));
        }
    }

    bool get_reuseaddr() const override {
        if(_fd) {
            return _fd.get_file_desc().getsockopt<int>(SOL_SOCKET, SO_REUSEADDR);
        } else {
            return _reuseaddr;
        }
    }

    virtual void shutdown() override {
        if (_fd) {
            try {
                _fd.shutdown(SHUT_RDWR);
            } catch (std::system_error& e) {
                if (e.code().value() != ENOTCONN) {
                    throw;
                }
            }
        }
    }
};

static
unsigned
get_port_or_counter(const socket_address& sa) {
    static thread_local constinit unsigned counter = 0;
    switch (sa.family()) {
    case AF_INET:
        return ntoh(sa.as_posix_sockaddr_in().sin_port);
    case AF_INET6:
        return ntoh(sa.as_posix_sockaddr_in6().sin6_port);
    default:
        // AF_UNIX? Shouldn't happen but not worth raising an error for it
        return counter++;
    }
}

// Read exactly `len` bytes from fd into buf, looping on partial reads.
// Returns false on EOF before all bytes are read.
static
future<bool>
read_exactly_from_fd(pollable_fd& fd, char* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        auto n = co_await fd.read_some(buf + total, len - total);
        if (n == 0) {
            co_return false;
        }
        total += n;
    }
    co_return true;
}

static const char pp2_signature[12] = {
    0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a
};

// Parse PP v2 version/command, family/protocol, and addresses from a 16-byte
// header and an address payload buffer.  Returns nullopt on invalid data.
static
std::optional<proxy_data>
parse_pp2_addresses(const char* header, const char* addr_buf, uint16_t addr_len,
                    const socket_address& local_addr, const socket_address& remote_addr) {
    uint8_t fam_proto = header[13];
    switch (header[12]) { // version and command
    case 0x20: // v2, LOCAL
        if (fam_proto != 0x00) { // UNSPEC
            return std::nullopt;
        }
        return proxy_data{.remote_address = remote_addr, .local_address = local_addr};
    case 0x21: // v2, PROXY
        break;
    default:
        return std::nullopt;
    }

    auto fam = fam_proto >> 4;
    auto proto = fam_proto & 0x0F;

    // We could, in principle, support DGRAM here, but there's no
    // real need for it. Real-world proxies support proxying UDP as UDP.
    if (proto != 0x1) { // STREAM
        return std::nullopt;
    }

    proxy_data addr_data;

    switch (fam) {
    case 0x1: { // INET
        if (addr_len < 12) {
            return std::nullopt;
        }
        addr_data.remote_address = ipv4_addr(inet_address(copy_reinterpret_cast<in_addr>(addr_buf)), read_be<uint16_t>(addr_buf + 8));
        addr_data.local_address = ipv4_addr(inet_address(copy_reinterpret_cast<in_addr>(addr_buf + 4)), read_be<uint16_t>(addr_buf + 10));
        break;
    }
    case 0x2: { // INET6
        if (addr_len < 36) {
            return std::nullopt;
        }
        addr_data.remote_address = ipv6_addr(inet_address(copy_reinterpret_cast<in6_addr>(addr_buf)), read_be<uint16_t>(addr_buf + 32));
        addr_data.local_address = ipv6_addr(inet_address(copy_reinterpret_cast<in6_addr>(addr_buf + 16)), read_be<uint16_t>(addr_buf + 34));
        break;
    }
    default:
        return std::nullopt;
    }
    return addr_data;
}

// Data source that yields prefix bytes before delegating to the underlying source.
// Used by unified port to prepend bytes consumed during protocol detection.
class prefixed_data_source_impl final : public data_source_impl {
    temporary_buffer<char> _prefix;
    data_source _underlying;
public:
    prefixed_data_source_impl(temporary_buffer<char> prefix, data_source underlying)
        : _prefix(std::move(prefix)), _underlying(std::move(underlying)) {}
    future<temporary_buffer<char>> get() override {
        if (!_prefix.empty()) {
            return make_ready_future<temporary_buffer<char>>(std::move(_prefix));
        }
        return _underlying.get();
    }
    future<temporary_buffer<char>> skip(uint64_t n) override {
        if (!_prefix.empty()) {
            auto skip_bytes = std::min(n, static_cast<uint64_t>(_prefix.size()));
            _prefix.trim_front(skip_bytes);
            n -= skip_bytes;
        }
        if (n == 0) {
            return make_ready_future<temporary_buffer<char>>();
        }
        return _underlying.skip(n);
    }
    future<> close() override {
        return _underlying.close();
    }
};

// Connected socket that prepends a single prefix byte to the data stream
// and optionally carries proxy protocol address data.
// Used by unified port for legacy clients where the first byte was consumed during detection.
// When proxy data is present, local_address() and remote_address() return the proxied addresses.
class prefixed_posix_connected_socket_impl final : public posix_connected_socket_impl {
    std::optional<char> _prefix;
    std::optional<socket_address> _proxy_local_addr;
    std::optional<socket_address> _proxy_remote_addr;
public:
    prefixed_posix_connected_socket_impl(
            sa_family_t family, int protocol, pollable_fd fd,
            conntrack::handle&& handle,
            std::optional<char> prefix,
            std::optional<proxy_data> addr_data_opt = {},
            std::pmr::polymorphic_allocator<char>* allocator = memory::malloc_allocator)
        : posix_connected_socket_impl(family, protocol, std::move(fd), std::move(handle), allocator)
        , _prefix(prefix)
    {
        if (addr_data_opt) {
            _proxy_local_addr = std::move(addr_data_opt->local_address);
            _proxy_remote_addr = std::move(addr_data_opt->remote_address);
        }
    }
    data_source source() override {
        return source(connected_socket_input_stream_config());
    }
    data_source source(connected_socket_input_stream_config csisc) override {
        auto underlying = posix_connected_socket_impl::source(csisc);
        if (!_prefix) {
            return underlying;
        }
        temporary_buffer<char> pfx(1);
        *pfx.get_write() = *_prefix;
        _prefix.reset(); // consume prefix on first call only
        return data_source(std::make_unique<prefixed_data_source_impl>(
            std::move(pfx), std::move(underlying)));
    }
    socket_address local_address() const noexcept override {
        return _proxy_local_addr ? *_proxy_local_addr : posix_connected_socket_impl::local_address();
    }
    socket_address remote_address() const noexcept override {
        return _proxy_remote_addr ? *_proxy_remote_addr : posix_connected_socket_impl::remote_address();
    }
};

// Result of reading and parsing a PP v2 header.
struct pp2_parse_result {
    enum class status { ok, incomplete, invalid };
    status st;
    std::optional<proxy_data> addr_data;
};

// Read and parse a PP v2 header from a validated 16-byte header buffer.
// `read_fn` must read exactly `n` bytes into `buf` and return false on failure.
// This function reads the address payload, validates it, and returns the result.
template <typename ReadFn>
static future<pp2_parse_result>
read_and_parse_pp2(const char* full_header, ReadFn&& read_fn,
                   const socket_address& local_addr, const socket_address& remote_addr) {
    auto len = read_be<uint16_t>(full_header + 14);
    static constexpr uint16_t max_pp2_addr_len = 256;
    if (len > max_pp2_addr_len) {
        co_return pp2_parse_result{pp2_parse_result::status::invalid, std::nullopt};
    }
    // Stack buffer covers all valid address lengths (max 256 bytes).
    char buffer[max_pp2_addr_len];
    if (!co_await read_fn(buffer, len)) {
        co_return pp2_parse_result{pp2_parse_result::status::incomplete, std::nullopt};
    }
    auto addr = parse_pp2_addresses(full_header, buffer, len, local_addr, remote_addr);
    if (!addr) {
        co_return pp2_parse_result{pp2_parse_result::status::invalid, std::nullopt};
    }
    co_return pp2_parse_result{pp2_parse_result::status::ok, std::move(addr)};
}

// Timeout for the protocol detection phase on unified port connections.
// If a client connects but doesn't send data within this period, the
// connection is dropped so it doesn't block the accept loop.
static constexpr auto unified_port_detect_timeout = std::chrono::seconds(5);

static
std::unique_ptr<connected_socket_impl>
make_connected_socket_impl(
        sa_family_t family,
        int protocol,
        pollable_fd fd,
        conntrack::handle&& handle,
        std::optional<proxy_data> addr_data_opt,
        std::optional<char> prefix = {},
        std::pmr::polymorphic_allocator<char>* allocator = memory::malloc_allocator) {
    if (prefix) {
        // Handles both prefix-only and prefix + proxy data (PP v2 + legacy CQL).
        return std::make_unique<prefixed_posix_connected_socket_impl>(
            family, protocol, std::move(fd), std::move(handle),
            prefix, std::move(addr_data_opt), allocator);
    }
    if (addr_data_opt) {
        return std::make_unique<posix_proxied_connected_socket_impl>(
            family,
            protocol,
            std::move(fd),
            std::move(handle),
            std::move(addr_data_opt->local_address),
            std::move(addr_data_opt->remote_address),
            allocator);
    }
    return std::make_unique<posix_connected_socket_impl>(
        family,
        protocol,
        std::move(fd),
        std::move(handle),
        allocator);
}

// Result of unified port protocol detection phase.
// Returned by detect_unified_port_protocol() and consumed by both
// posix_server_socket_impl::accept() and posix_reuseport_server_socket_impl::accept().
struct protocol_detection_result {
    enum class outcome { success, drop };
    outcome status = outcome::drop;
    std::optional<proxy_data> addr_data;
    connection_metadata meta;
    std::optional<char> prefix;
    bool has_ppv2 = false;
    bool has_meta_frame = false;
};

// Perform unified port protocol detection on a newly-accepted connection.
// Reads PPv2 header and/or first byte for TLS detection depending on config.
// Does NOT peek into the data stream unless it has to:
//   - ppv2 auto_detect: reads first byte to check for 0x0D
//   - detect_tls: reads first byte to check for 0x16
//   - neither: no reads at all
// On failure (timeout, invalid data), returns outcome::drop.
// `sa` may be updated to the proxied remote address when PPv2 is present.
static future<protocol_detection_result>
detect_unified_port_protocol(const posix_server_socket_impl::accept_config& cfg,
                             pollable_fd& fd, socket_address& sa) {
    using accept_cfg = posix_server_socket_impl::accept_config;
    using result_t = protocol_detection_result;
    result_t res;
    static thread_local logger::rate_limit drop_rl(std::chrono::seconds(10));

    auto deadline = lowres_clock::now() + unified_port_detect_timeout;

    auto read_with_timeout = [&](char* buf, size_t len) -> future<bool> {
        try {
            co_return co_await with_timeout(deadline, read_exactly_from_fd(fd, buf, len));
        } catch (const timed_out_error&) {
            seastar_logger.debug("accept: timeout during protocol detection, dropping connection");
            co_return false;
        }
    };

    auto drop_connection = [&](const char* reason) {
        seastar_logger.debug("accept: {}, dropping connection from {}", reason, sa);
        seastar_logger.log(log_level::warn, drop_rl,
            "accept: dropping invalid connection from {} ({})", sa, reason);
    };

    // --- PPv2 mandatory mode (dedicated proxy protocol ports) ---
    bool ppv2_read_done = false;
    if (cfg.ppv2 == accept_cfg::ppv2_mode::mandatory) {
        char full_header[16];
        if (!co_await read_with_timeout(full_header, 16)) {
            drop_connection("incomplete PP v2 header");
            co_return res;
        }
        if (std::memcmp(full_header, pp2_signature, sizeof(pp2_signature)) != 0) {
            drop_connection("invalid PP v2 signature");
            co_return res;
        }
        auto result = co_await read_and_parse_pp2(full_header, read_with_timeout,
            fd.get_file_desc().get_address(), fd.get_file_desc().get_remote_address());
        if (result.st == pp2_parse_result::status::incomplete) {
            drop_connection("incomplete PP v2 address data");
            co_return res;
        }
        if (result.st == pp2_parse_result::status::invalid) {
            drop_connection("unsupported PP v2 address family or protocol");
            co_return res;
        }
        res.addr_data = std::move(result.addr_data);
        sa = res.addr_data->remote_address;
        res.has_ppv2 = true;
        ppv2_read_done = true;
    }

    // Determine if we need to read the first byte (after mandatory PPv2, or
    // as the very first byte of the connection).
    bool need_post_ppv2 = cfg.detect_tls || cfg.detect_meta_frame;
    bool need_first_byte = (!ppv2_read_done && cfg.ppv2 == accept_cfg::ppv2_mode::auto_detect)
                        || need_post_ppv2
                        || (ppv2_read_done && need_post_ppv2);

    char first_byte;
    bool have_first_byte = false;
    if (need_first_byte) {
        if (!co_await read_with_timeout(&first_byte, 1)) {
            drop_connection(ppv2_read_done ? "EOF after PP v2 header" : "EOF or timeout on first byte");
            co_return res;
        }
        have_first_byte = true;
    }

    // --- PPv2 auto-detect from first byte (0x0D) ---
    if (have_first_byte && !ppv2_read_done
            && cfg.ppv2 == accept_cfg::ppv2_mode::auto_detect
            && first_byte == 0x0D) {
        char header_rest[15];
        if (!co_await read_with_timeout(header_rest, 15)) {
            drop_connection("incomplete PP v2 header");
            co_return res;
        }
        char full_header[16];
        full_header[0] = first_byte;
        std::memcpy(full_header + 1, header_rest, 15);
        if (std::memcmp(full_header, pp2_signature, sizeof(pp2_signature)) != 0) {
            drop_connection("invalid PP v2 signature");
            co_return res;
        }
        auto result = co_await read_and_parse_pp2(full_header, read_with_timeout,
            fd.get_file_desc().get_address(), fd.get_file_desc().get_remote_address());
        if (result.st == pp2_parse_result::status::incomplete) {
            drop_connection("incomplete PP v2 address data");
            co_return res;
        }
        if (result.st == pp2_parse_result::status::invalid) {
            drop_connection("unsupported PP v2 address family or protocol");
            co_return res;
        }
        res.addr_data = std::move(result.addr_data);
        sa = res.addr_data->remote_address;
        res.has_ppv2 = true;

        // After PPv2, read next byte if TLS or meta-frame detection is needed
        if (need_post_ppv2) {
            if (!co_await read_with_timeout(&first_byte, 1)) {
                drop_connection("EOF after PP v2 header");
                co_return res;
            }
            // have_first_byte remains true — first_byte now holds post-PPv2 byte
        } else {
            have_first_byte = false; // PPv2 consumed, no further peeking
        }
    }

    // --- Meta-frame detection (magic byte 0x53) ---
    // Wire format: [0x53 magic] [4-byte BE version] [version-specific payload]
    // Version 1 payload: [2-byte BE target shard]
    // Total: 7 bytes. Fully consumed (not prepended as prefix).
    if (have_first_byte && cfg.detect_meta_frame
            && static_cast<uint8_t>(first_byte) == 0x53) {
        char frame_rest[6]; // 4-byte version + 2-byte shard
        if (!co_await read_with_timeout(frame_rest, 6)) {
            drop_connection("incomplete meta-frame");
            co_return res;
        }
        uint32_t version = (static_cast<uint8_t>(frame_rest[0]) << 24)
                         | (static_cast<uint8_t>(frame_rest[1]) << 16)
                         | (static_cast<uint8_t>(frame_rest[2]) << 8)
                         | static_cast<uint8_t>(frame_rest[3]);
        if (version != 1) {
            drop_connection(seastar::format("unsupported meta-frame version {}", version).c_str());
            co_return res;
        }
        uint16_t target_shard = (static_cast<uint8_t>(frame_rest[4]) << 8)
                              | static_cast<uint8_t>(frame_rest[5]);
        if (target_shard >= smp::count) {
            drop_connection(seastar::format("meta-frame shard {} out of range (max {})",
                target_shard, smp::count - 1).c_str());
            co_return res;
        }
        res.meta.meta_frame_shard = target_shard;
        res.has_meta_frame = true;

        // Meta-frame fully consumed. Read next byte for TLS detection if needed.
        if (cfg.detect_tls) {
            if (!co_await read_with_timeout(&first_byte, 1)) {
                drop_connection("EOF after meta-frame");
                co_return res;
            }
            // have_first_byte remains true — first_byte now holds post-meta-frame byte
        } else {
            have_first_byte = false;
        }
    }

    // --- TLS detection / prefix handling ---
    if (have_first_byte) {
        if (cfg.detect_tls) {
            res.meta.is_tls = (static_cast<uint8_t>(first_byte) == 0x16);
        }
        res.prefix = first_byte;
    }

    res.status = result_t::outcome::success;
    co_return res;
}

// Runs on a remote shard: performs protocol detection on a forwarded raw fd,
// then delivers the completed connection to the local posix_ap_server_socket_impl.
static future<> detect_and_deliver_connection(
        int protocol, socket_address server_addr,
        file_desc raw_fd, socket_address remote_addr,
        conntrack::handle cth,
        posix_server_socket_impl::accept_config cfg,
        std::pmr::polymorphic_allocator<char>* allocator) {
    auto fd = pollable_fd(std::move(raw_fd));
    auto det = co_await detect_unified_port_protocol(cfg, fd, remote_addr);
    if (det.status == protocol_detection_result::outcome::drop) {
        co_return;
    }
    posix_ap_server_socket_impl::move_connected_socket(
        protocol, server_addr, std::move(fd), remote_addr, std::move(cth),
        std::move(det.addr_data), allocator, det.meta, det.prefix);
}

conntrack::handle
posix_server_socket_impl::route_connection(const accept_config& cfg,
        bool has_ppv2, std::optional<shard_id> meta_frame_shard,
        const socket_address& sa) {
    // Meta-frame shard takes highest priority — the driver explicitly requested it.
    if (meta_frame_shard) {
        return _conntrack.get_handle(*meta_frame_shard);
    }
    auto lba = has_ppv2 ? cfg.ppv2_lba : cfg.default_lba;
    switch (lba) {
    case server_socket::load_balancing_algorithm::port:
        return _conntrack.get_handle(get_port_or_counter(sa) % smp::count);
    case server_socket::load_balancing_algorithm::fixed:
        return _conntrack.get_handle(cfg.fixed_cpu);
    case server_socket::load_balancing_algorithm::connection_distribution:
    default:
        return _conntrack.get_handle();
    }
}

future<accept_result>
posix_server_socket_impl::accept() {
    const bool needs_detection = _accept_cfg.needs_detection();
    const bool distribute = _accept_cfg.distribute_detection;

    while (true) {
        co_await coroutine::maybe_yield();
        auto [fd, sa] = co_await _lfd.accept();

        if (distribute) {
            // Distribute detection work across shards. Pick a round-robin
            // shard and forward the raw fd there. The detection shard does
            // the protocol sniffing and keeps the connection.
            auto cth = _conntrack.get_handle(); // round-robin
            auto cpu = cth.cpu();
            if (cpu == this_shard_id()) {
                // Detection on local shard — do it here and return.
                auto det = co_await detect_unified_port_protocol(_accept_cfg, fd, sa);
                if (det.status == protocol_detection_result::outcome::drop) {
                    continue;
                }
                auto csi = make_connected_socket_impl(
                    sa.family(), _protocol, std::move(fd), std::move(cth),
                    std::move(det.addr_data), det.prefix, _allocator);
                co_return accept_result{connected_socket(std::move(csi)), sa, det.meta};
            } else {
                // Forward raw fd to remote shard for detection + delivery.
                (void)smp::submit_to(cpu, [protocol = _protocol, ssa = _sa,
                        raw_fd = std::move(fd.get_file_desc()), sa,
                        cth = std::move(cth), cfg = _accept_cfg,
                        allocator = _allocator] () mutable {
                    return detect_and_deliver_connection(
                        protocol, ssa, std::move(raw_fd), sa,
                        std::move(cth), cfg, allocator);
                }).handle_exception([sa] (auto ep) {
                    seastar_logger.warn("accept: failed to distribute detection for connection from {}: {}", sa, ep);
                });
            }
            continue;
        }

        // Non-distributed path: detect locally, then route.
        std::optional<proxy_data> addr_data_opt;
        connection_metadata meta;
        std::optional<char> prefix;
        bool has_ppv2 = false;

        if (needs_detection) {
            auto det = co_await detect_unified_port_protocol(_accept_cfg, fd, sa);
            if (det.status == protocol_detection_result::outcome::drop) {
                continue;
            }
            addr_data_opt = std::move(det.addr_data);
            meta = det.meta;
            prefix = det.prefix;
            has_ppv2 = det.has_ppv2;
        }

        auto cth = route_connection(_accept_cfg, has_ppv2, meta.meta_frame_shard, sa);
        auto cpu = cth.cpu();
        if (cpu == this_shard_id()) {
            auto csi = make_connected_socket_impl(
                sa.family(), _protocol, std::move(fd), std::move(cth),
                std::move(addr_data_opt), prefix, _allocator);
            co_return accept_result{connected_socket(std::move(csi)), sa, meta};
        } else {
            (void)smp::submit_to(cpu, [protocol = _protocol, ssa = _sa,
                    fd = std::move(fd.get_file_desc()), sa, cth = std::move(cth),
                    addr_data_opt = std::move(addr_data_opt), allocator = _allocator,
                    meta, prefix] () mutable {
                posix_ap_server_socket_impl::move_connected_socket(
                    protocol, ssa, pollable_fd(std::move(fd)), sa, std::move(cth),
                    std::move(addr_data_opt), allocator, meta, prefix);
            }).handle_exception([sa] (auto ep) {
                seastar_logger.warn("accept: failed to move connection from {} to target shard: {}", sa, ep);
            });
        }
    }
}

void
posix_server_socket_impl::abort_accept() {
    _lfd.shutdown(SHUT_RD, pollable_fd::shutdown_kernel_only::no);
}

socket_address posix_server_socket_impl::local_address() const {
    return _lfd.get_file_desc().get_address();
}

posix_ap_server_socket_impl::posix_ap_server_socket_impl(int protocol, socket_address sa, std::pmr::polymorphic_allocator<char>* allocator)
        : _protocol(protocol), _sa(sa), _allocator(allocator)
{
    auto it = ports.emplace(std::make_tuple(_protocol, _sa));
    if (!it.second) {
        throw std::system_error(EADDRINUSE, std::system_category());
    }
}

posix_ap_server_socket_impl::~posix_ap_server_socket_impl() {
    ports.erase(std::make_tuple(_protocol, _sa));
}

future<accept_result> posix_ap_server_socket_impl::accept() {
    auto t_sa = std::make_tuple(_protocol, _sa);
    auto conni = conn_q.find(t_sa);
    if (conni != conn_q.end()) {
        connection c = std::move(conni->second);
        conn_q.erase(conni);
        try {
            auto csi = make_connected_socket_impl(
                _sa.family(), _protocol, std::move(c.fd),
                std::move(c.connection_tracking_handle),
                std::move(c.proxy_protocol_header_opt),
                c.prefix, _allocator);
            return make_ready_future<accept_result>(accept_result{connected_socket(std::move(csi)), std::move(c.addr), c.metadata});
        } catch (...) {
            return make_exception_future<accept_result>(std::current_exception());
        }
    } else {
        try {
            auto i = sockets.emplace(std::piecewise_construct, std::make_tuple(t_sa), std::make_tuple());
            SEASTAR_ASSERT(i.second);
            return i.first->second.get_future();
        } catch (...) {
            return make_exception_future<accept_result>(std::current_exception());
        }
    }
}

void
posix_ap_server_socket_impl::abort_accept() {
    auto t_sa = std::make_tuple(_protocol, _sa);
    conn_q.erase(t_sa);
    auto i = sockets.find(t_sa);
    if (i != sockets.end()) {
        i->second.set_exception(std::system_error(ECONNABORTED, std::system_category()));
        sockets.erase(i);
    }
}

future<accept_result>
posix_reuseport_server_socket_impl::accept() {
    const bool needs_detection = _accept_cfg.needs_detection();

    while (true) {
        co_await coroutine::maybe_yield();
        auto [fd, sa] = co_await _lfd.accept();

        if (!needs_detection) {
            // Fast path: no protocol detection, serve on this shard directly
            std::unique_ptr<connected_socket_impl> csi(
                    new posix_connected_socket_impl(sa.family(), _protocol, std::move(fd), _allocator));
            co_return accept_result{connected_socket(std::move(csi)), sa};
        }

        // Unified port protocol detection
        auto det = co_await detect_unified_port_protocol(_accept_cfg, fd, sa);
        if (det.status == protocol_detection_result::outcome::drop) {
            continue;
        }

        // Meta-frame or PPv2 shard-aware routing: forward the connection if it
        // belongs on a different shard.
        auto forward_to_shard = [&]() -> std::optional<shard_id> {
            if (det.has_meta_frame && det.meta.meta_frame_shard) {
                return *det.meta.meta_frame_shard;
            }
            if (det.has_ppv2
                    && _accept_cfg.ppv2_lba == server_socket::load_balancing_algorithm::port) {
                return get_port_or_counter(sa) % smp::count;
            }
            return std::nullopt;
        }();
        if (forward_to_shard && *forward_to_shard != this_shard_id()) {
            auto cth = _conntrack.get_handle(*forward_to_shard);
            auto cpu = cth.cpu();
            (void)smp::submit_to(cpu, [protocol = _protocol, ssa = _sa,
                    fd = std::move(fd.get_file_desc()), sa, cth = std::move(cth),
                    addr_data_opt = std::move(det.addr_data), allocator = _allocator,
                    meta = det.meta, prefix = det.prefix] () mutable {
                posix_ap_server_socket_impl::move_connected_socket(
                    protocol, ssa, pollable_fd(std::move(fd)), sa, std::move(cth),
                    std::move(addr_data_opt), allocator, meta, prefix);
            }).handle_exception([sa] (auto ep) {
                seastar_logger.warn("accept: failed to move connection from {} to target shard: {}", sa, ep);
            });
            continue; // forwarded — accept next connection
        }

        // Connection stays on this shard
        auto cth = _conntrack.get_handle(this_shard_id());
        auto csi = make_connected_socket_impl(
            sa.family(), _protocol, std::move(fd), std::move(cth),
            std::move(det.addr_data), det.prefix, _allocator);
        co_return accept_result{connected_socket(std::move(csi)), sa, det.meta};
    }
}

void
posix_reuseport_server_socket_impl::abort_accept() {
    _lfd.shutdown(SHUT_RD, pollable_fd::shutdown_kernel_only::no);
}

socket_address posix_reuseport_server_socket_impl::local_address() const {
    return _lfd.get_file_desc().get_address();
}

void
posix_ap_server_socket_impl::move_connected_socket(int protocol, socket_address sa, pollable_fd fd, socket_address addr, conntrack::handle cth, std::optional<proxy_data> addr_data_opt, std::pmr::polymorphic_allocator<char>* allocator, connection_metadata meta, std::optional<char> prefix) {
    auto t_sa = std::make_tuple(protocol, sa);
    auto i = sockets.find(t_sa);
    if (i != sockets.end()) {
        try {
            auto csi = make_connected_socket_impl(
                sa.family(), protocol, std::move(fd), std::move(cth),
                std::move(addr_data_opt), prefix, allocator);
            i->second.set_value(accept_result{connected_socket(std::move(csi)), std::move(addr), meta});
        } catch (...) {
            i->second.set_exception(std::current_exception());
        }
        sockets.erase(i);
    } else {
        conn_q.emplace(std::piecewise_construct, std::make_tuple(t_sa), std::make_tuple(std::move(fd), std::move(addr), std::move(cth), std::move(addr_data_opt), meta, prefix));
    }
}

future<temporary_buffer<char>>
posix_data_source_impl::get() {
    return _fd.recv_some(static_cast<internal::buffer_allocator*>(this)).then([this] (temporary_buffer<char> b) {
        if (b.size() >= _config.buffer_size) {
            _config.buffer_size *= 2;
            _config.buffer_size = std::min(_config.buffer_size, _config.max_buffer_size);
        } else if (b.size() <= _config.buffer_size / 4) {
            _config.buffer_size /= 2;
            _config.buffer_size = std::max(_config.buffer_size, _config.min_buffer_size);
        }
        auto sg_id = internal::scheduling_group_index(current_scheduling_group());
        bytes_received[sg_id] += b.size();
        return b;
    });
}

temporary_buffer<char>
posix_data_source_impl::allocate_buffer() {
    return make_temporary_buffer<char>(_buffer_allocator, _config.buffer_size);
}

future<> posix_data_source_impl::close() {
    _fd.shutdown(SHUT_RD);
    return make_ready_future<>();
}

#if SEASTAR_API_LEVEL >= 9
future<> posix_data_sink_impl::put(std::span<temporary_buffer<char>> bufs) {
    auto [ total, del ] = _vecs.populate(bufs);
    auto sg_id = internal::scheduling_group_index(current_scheduling_group());
    bytes_sent[sg_id] += total;
    return _fd.write_all(_vecs.v).finally([del = std::move(del)] {} );
}
#else
future<>
posix_data_sink_impl::put(temporary_buffer<char> buf) {
    auto sg_id = internal::scheduling_group_index(current_scheduling_group());
    bytes_sent[sg_id] += buf.size();
    return _fd.write_all(buf.get(), buf.size()).then([d = buf.release()] {});
}

future<>
posix_data_sink_impl::put(packet p) {
    SEASTAR_ASSERT(!_p);
    _p = std::move(p);
    auto sg_id = internal::scheduling_group_index(current_scheduling_group());
    bytes_sent[sg_id] += _p.len();
    return _fd.write_all(_p).finally([this] { _p.reset(); });
}
#endif

future<>
posix_data_sink_impl::close() {
    _fd.shutdown(SHUT_WR);
    return make_ready_future<>();
}

void posix_data_sink_impl::on_batch_flush_error() noexcept {
    shutdown_socket_fd(_fd, SHUT_RD);
}

posix_network_stack::posix_network_stack(const program_options::option_group& opts, std::pmr::polymorphic_allocator<char>* allocator)
        : _reuseport(engine().posix_reuseport_available()), _allocator(allocator) {
}

static posix_server_socket_impl::accept_config
make_accept_config(const listen_options& opt) {
    using cfg_t = posix_server_socket_impl::accept_config;
    cfg_t cfg;
    if (opt.unified_port.enabled) {
        cfg.ppv2 = opt.unified_port.ppv2 ? cfg_t::ppv2_mode::auto_detect : cfg_t::ppv2_mode::disabled;
        cfg.detect_tls = opt.unified_port.detect_tls;
        cfg.detect_meta_frame = opt.unified_port.detect_meta_frame;
        cfg.default_lba = opt.unified_port.default_lba;
        cfg.ppv2_lba = opt.unified_port.ppv2_lba;
        // Distribute detection across shards when routing doesn't depend on
        // detection results. With ppv2 shard-aware routing or meta-frame,
        // we must detect first to know which shard to route to.
        cfg.distribute_detection = cfg.needs_detection()
            && opt.unified_port.ppv2_lba != server_socket::load_balancing_algorithm::port
            && !opt.unified_port.detect_meta_frame;
    } else if (opt.proxy_protocol) {
        cfg.ppv2 = cfg_t::ppv2_mode::mandatory;
        cfg.default_lba = opt.lba;
        cfg.ppv2_lba = opt.lba;
        cfg.fixed_cpu = opt.fixed_cpu;
    } else {
        cfg.default_lba = opt.lba;
        cfg.fixed_cpu = opt.fixed_cpu;
    }
    return cfg;
}

server_socket
posix_network_stack::listen(socket_address sa, listen_options opt) {
    using server_socket = seastar::server_socket;
    if (opt.unified_port.enabled && opt.proxy_protocol) {
        throw std::invalid_argument("unified_port and proxy_protocol are mutually exclusive");
    }
    // allow unspecified bind address -> default to ipv4 wildcard
    if (sa.is_unspecified()) {
        sa = inet_address(inet_address::family::INET);
    }
    auto cfg = make_accept_config(opt);
    if (sa.is_af_unix()) {
        return server_socket(std::make_unique<posix_server_socket_impl>(0, sa, internal::posix_listen(sa, opt), cfg, _allocator));
    }
    auto protocol = static_cast<int>(opt.proto);
    return _reuseport ?
        server_socket(std::make_unique<posix_reuseport_server_socket_impl>(protocol, sa, internal::posix_listen(sa, opt), cfg, _allocator))
        :
        server_socket(std::make_unique<posix_server_socket_impl>(protocol, sa, internal::posix_listen(sa, opt), cfg, _allocator));
}

::seastar::socket posix_network_stack::socket() {
    return ::seastar::socket(std::make_unique<posix_socket_impl>(_allocator));
}

posix_ap_network_stack::posix_ap_network_stack(const program_options::option_group& opts, std::pmr::polymorphic_allocator<char>* allocator)
        : posix_network_stack(opts, allocator) {
}

server_socket
posix_ap_network_stack::listen(socket_address sa, listen_options opt) {
    using server_socket = seastar::server_socket;
    if (opt.unified_port.enabled) {
        throw std::invalid_argument("unified_port is not supported on posix_ap_network_stack; use posix_network_stack instead");
    }
    // allow unspecified bind address -> default to ipv4 wildcard
    if (sa.is_unspecified()) {
        sa = inet_address(inet_address::family::INET);
    }
    if (sa.is_af_unix()) {
        return server_socket(std::make_unique<posix_ap_server_socket_impl>(0, sa, _allocator));
    }
    auto protocol = static_cast<int>(opt.proto);
    return _reuseport ?
        server_socket(std::make_unique<posix_reuseport_server_socket_impl>(protocol, sa, internal::posix_listen(sa, opt), posix_server_socket_impl::accept_config{}, _allocator))
        :
        server_socket(std::make_unique<posix_ap_server_socket_impl>(protocol, sa, _allocator));
}

struct cmsg_with_pktinfo {
    struct cmsghdrcmh;
    union {
        struct in_pktinfo pktinfo;
        struct in6_pktinfo pkt6info;
    };
};

class posix_datagram_channel : public datagram_channel_impl {
private:
    static constexpr int MAX_DATAGRAM_SIZE = 65507;
    struct recv_ctx {
        struct msghdr _hdr;
        struct iovec _iov;
        socket_address _src_addr;
        char* _buffer;
        cmsg_with_pktinfo _cmsg;

        recv_ctx(bool use_pktinfo) {
            memset(&_hdr, 0, sizeof(_hdr));
            _hdr.msg_iov = &_iov;
            _hdr.msg_iovlen = 1;
            _hdr.msg_name = &_src_addr.u.sa;
            _hdr.msg_namelen = sizeof(_src_addr.u.sas);

            if (use_pktinfo) {
                memset(&_cmsg, 0, sizeof(_cmsg));
                _hdr.msg_control = &_cmsg;
                _hdr.msg_controllen = sizeof(_cmsg);
            } else {
                _hdr.msg_control = nullptr;
                _hdr.msg_controllen = 0;
            }
        }

        recv_ctx(const recv_ctx&) = delete;
        recv_ctx(recv_ctx&&) = delete;

        void prepare() {
            _buffer = new char[MAX_DATAGRAM_SIZE];
            _iov.iov_base = _buffer;
            _iov.iov_len = MAX_DATAGRAM_SIZE;
        }
    };
    struct send_ctx {
        struct msghdr _hdr;
        internal::wrapped_iovecs _vecs;
        socket_address _dst;

        send_ctx() {
            memset(&_hdr, 0, sizeof(_hdr));
            _hdr.msg_name = &_dst.u.sa;
            _hdr.msg_namelen = _dst.addr_length;
        }

        send_ctx(const send_ctx&) = delete;
        send_ctx(send_ctx&&) = delete;

        auto prepare(const socket_address& dst, std::span<temporary_buffer<char>> bufs) {
            _dst = dst;
            _hdr.msg_namelen = _dst.addr_length;
            auto ret = _vecs.populate(bufs);
            _hdr.msg_iov = _vecs.v.data();
            _hdr.msg_iovlen = _vecs.v.size();
            resolve_outgoing_address(_dst);
            return ret;
        }
    };

    static bool is_inet(sa_family_t family) {
        return family == AF_INET || family == AF_INET6;
    }

    static file_desc create_socket(sa_family_t family) {
        file_desc fd = file_desc::socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

        if (is_inet(family)) {
            fd.setsockopt(SOL_IP, IP_PKTINFO, true);
            if (engine().posix_reuseport_available()) {
                fd.setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
            }
        }

        return fd;
    }

    pollable_fd _fd;
    socket_address _address;
    recv_ctx _recv;
    send_ctx _send;
    bool _closed;
public:
    /// Creates a channel that is not bound to any socket address. The channel
    /// can be used to communicate with adressess that belong to the \param
    /// family.
    posix_datagram_channel(sa_family_t family)
        : _recv(is_inet(family)), _closed(false) {
        auto fd = create_socket(family);

        _address = fd.get_address();
        _fd = std::move(fd);
    }

    /// Creates a channel that is bound to the specified local address. It can be used to
    /// communicate with addresses that belong to the family of \param local.
    posix_datagram_channel(socket_address local)
        : _recv(is_inet(local.family())), _closed(false) {
        auto fd = create_socket(local.family());
        fd.bind(local.u.sa, local.addr_length);

        _address = fd.get_address();
        _fd = std::move(fd);
    }

    virtual ~posix_datagram_channel() { if (!_closed) close(); };
    virtual future<datagram> receive() override;
    virtual future<> send(const socket_address& dst, const char *msg) override;
    virtual future<> send(const socket_address& dst, std::span<temporary_buffer<char>> bufs) override;
    virtual void shutdown_input() override {
        _fd.shutdown(SHUT_RD, pollable_fd::shutdown_kernel_only::no);
    }
    virtual void shutdown_output() override {
        _fd.shutdown(SHUT_WR, pollable_fd::shutdown_kernel_only::no);
    }
    virtual void close() override {
        _closed = true;
        _fd = {};
    }
    virtual bool is_closed() const override { return _closed; }
    socket_address local_address() const override {
        SEASTAR_ASSERT(_address.u.sas.ss_family != AF_INET6 || (_address.addr_length > 20));
        return _address;
    }
};

future<> posix_datagram_channel::send(const socket_address& dst, const char *message) {
    auto len = strlen(message);
    auto a = dst;
    resolve_outgoing_address(a);
    auto sg_id = internal::scheduling_group_index(current_scheduling_group());
    bytes_sent[sg_id] += len;
    return _fd.sendto(a, message, len)
            .then([len] (size_t size) { SEASTAR_ASSERT(size == len); });
}

future<> posix_datagram_channel::send(const socket_address& dst, std::span<temporary_buffer<char>> bufs) {
    auto sg_id = internal::scheduling_group_index(current_scheduling_group());
    auto [ len, del ] = _send.prepare(dst, bufs);
    bytes_sent[sg_id] += len;
    return _fd.sendmsg(&_send._hdr)
            .then([len, del = std::move(del) ] (size_t size) { SEASTAR_ASSERT(size == len); });
}

udp_channel
posix_network_stack::make_udp_channel(const socket_address& addr) {
    if (!addr.is_unspecified()) {
        return make_bound_datagram_channel(addr);
    } else {
        // Preserve the default behavior of make_udp_channel({}) which is to
        // create an unbound channel that can be used to send UDP datagrams.
        return make_unbound_datagram_channel(AF_INET);
    }
}

datagram_channel
posix_network_stack::make_unbound_datagram_channel(sa_family_t family) {
    return datagram_channel(std::make_unique<posix_datagram_channel>(family));
}

datagram_channel
posix_network_stack::make_bound_datagram_channel(const socket_address& local) {
    return datagram_channel(std::make_unique<posix_datagram_channel>(local));
}

bool
posix_network_stack::supports_ipv6() const {
    static bool has_ipv6 = [] {
        try {
            posix_datagram_channel c(ipv6_addr{"::1"});
            c.close();
            return true;
        } catch (...) {}
        return false;
    }();

    return has_ipv6;
}

class posix_datagram : public datagram_impl {
private:
    socket_address _src;
    socket_address _dst;
    temporary_buffer<char> _buf;
public:
    posix_datagram(const socket_address& src, const socket_address& dst, temporary_buffer<char> b) : _src(src), _dst(dst), _buf(std::move(b)) {}
    virtual socket_address get_src() override { return _src; }
    virtual socket_address get_dst() override { return _dst; }
    virtual uint16_t get_dst_port() override {
        if (_dst.family() != AF_INET && _dst.family() != AF_INET6) {
            throw std::runtime_error(format("get_dst_port() called on non-IP address: {}", _dst));
        }
        return _dst.port();
    }
    virtual std::span<temporary_buffer<char>> get_buffers() override { return std::span(&_buf, 1); }
};

future<datagram>
posix_datagram_channel::receive() {
    _recv.prepare();
    return _fd.recvmsg(&_recv._hdr).then([this] (size_t size) {
        std::optional<socket_address> dst;
        for (auto* cmsg = CMSG_FIRSTHDR(&_recv._hdr); cmsg != nullptr; cmsg = CMSG_NXTHDR(&_recv._hdr, cmsg)) {
            if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
                dst = ipv4_addr(copy_reinterpret_cast<in_pktinfo>(CMSG_DATA(cmsg)).ipi_addr, _address.port());
                break;
            } else if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
                dst = ipv6_addr(copy_reinterpret_cast<in6_pktinfo>(CMSG_DATA(cmsg)).ipi6_addr, _address.port());
                break;
            }
        }
        auto sg_id = internal::scheduling_group_index(current_scheduling_group());
        bytes_received[sg_id] += size;
        return make_ready_future<datagram>(datagram(std::make_unique<posix_datagram>(
            _recv._src_addr, dst ? *dst : _address, temporary_buffer<char>(_recv._buffer, size, make_deleter([buf = _recv._buffer] { delete[] buf; })))));
    }).handle_exception([p = _recv._buffer](auto ep) {
        delete[] p;
        return make_exception_future<datagram>(std::move(ep));
    });
}

network_stack_entry register_posix_stack() {
    return network_stack_entry{
        "posix", std::make_unique<program_options::option_group>(nullptr, "Posix"),
        [](const program_options::option_group& ops) {
            return smp::main_thread() ? posix_network_stack::create(ops)
                                      : posix_ap_network_stack::create(ops);
        },
        true};
}

// nw interface stuff

std::vector<network_interface> posix_network_stack::network_interfaces() {
    class posix_network_interface_impl final : public network_interface_impl {
    public:
        uint32_t _index = 0, _mtu = 0;
        sstring _name, _display_name;
        std::vector<net::inet_address> _addresses;
        std::vector<uint8_t> _hardware_address;
        bool _loopback = false, _virtual = false, _up = false;

        uint32_t index() const override {
            return _index;
        }
        uint32_t mtu() const override {
            return _mtu;
        }
        const sstring& name() const override {
            return _name;
        }
        const sstring& display_name() const override {
            return _display_name.empty() ? name() : _display_name;
        }
        const std::vector<net::inet_address>& addresses() const override {
            return _addresses;
        }
        const std::vector<uint8_t> hardware_address() const override {
            return _hardware_address;
        }
        bool is_loopback() const override {
            return _loopback;
        }
        bool is_virtual() const override {
            return _virtual;
        }
        bool is_up() const override {
            // TODO: should be checked on query?
            return _up;
        }
        bool supports_ipv6() const override {
            // TODO: this is not 100% correct.
            return std::any_of(_addresses.begin(), _addresses.end(), std::mem_fn(&inet_address::is_ipv6));
        }
    };

    // For now, keep an immutable set of interfaces created on start, shared across
    // shards
    static const std::vector<posix_network_interface_impl> global_interfaces = [] {
        auto fd = ::socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        throw_system_error_on(fd < 0, "could not open netlink socket");

        std::unique_ptr<int, void(*)(int*)> fd_guard(&fd, [](int* p) { ::close(*p); });

        auto pid = ::getpid();

        sockaddr_nl local = {
          .nl_family = AF_NETLINK,
          .nl_pid = static_cast<unsigned int>(pid),
          .nl_groups = RTMGRP_IPV6_IFADDR|RTMGRP_IPV4_IFADDR,
        };

        throw_system_error_on(bind(fd, (struct sockaddr *) &local, sizeof(local)) < 0, "could not bind netlink socket");

        /* RTNL socket is ready for use, prepare and send requests */

        std::vector<posix_network_interface_impl> res;

        for (auto msg : { RTM_GETLINK, RTM_GETADDR}) {
            struct nl_req {
                nlmsghdr hdr;
                union {
                    rtgenmsg gen;
                    ifaddrmsg addr;
                };
            } req = {};

            req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg));
            req.hdr.nlmsg_type = msg;
            req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT;
            req.hdr.nlmsg_seq = 1;
            req.hdr.nlmsg_pid = pid;

            if (msg == RTM_GETLINK) {
                req.gen.rtgen_family = AF_PACKET; /*  no preferred AF, we will get *all* interfaces */
            } else {
                req.addr.ifa_family = AF_UNSPEC;
            }

            sockaddr_nl kernel = {
              .nl_family = AF_NETLINK, /* fill-in kernel address (destination of our message) */
            };
            iovec io = {
              .iov_base = &req,
              .iov_len = req.hdr.nlmsg_len,
            };
            msghdr rtnl_msg = {
              .msg_name = &kernel,
              .msg_namelen = sizeof(kernel),
              .msg_iov = &io,
              .msg_iovlen = 1,
            };

            throw_system_error_on(::sendmsg(fd, (struct msghdr *) &rtnl_msg, 0) < 0, "could not send netlink request");
            /* parse reply */

            constexpr size_t reply_buffer_size = 8192;
            char reply[reply_buffer_size];

            bool done = false;

            while (!done) {
                iovec io_reply = {
                  .iov_base = reply,
                  .iov_len = reply_buffer_size,
                };
                msghdr rtnl_reply = {
                  .msg_name = &kernel,
                  .msg_namelen = sizeof(kernel),
                  .msg_iov = &io_reply,
                  .msg_iovlen = 1,
                };

                auto len = ::recvmsg(fd, &rtnl_reply, 0); /* read as much data as fits in the receive buffer */
                if (len <= 0) {
                    return res;
                }

                for (auto* msg_ptr = (struct nlmsghdr *) reply; NLMSG_OK(msg_ptr, len); msg_ptr = NLMSG_NEXT(msg_ptr, len)) {
                    switch(msg_ptr->nlmsg_type) {
                    case NLMSG_DONE: // that is all
                        done = true;
                        break;
                    case RTM_NEWLINK:
                    {
                        auto* iface = reinterpret_cast<const ifinfomsg*>(NLMSG_DATA(msg_ptr));
                        auto ilen = msg_ptr->nlmsg_len - NLMSG_LENGTH(sizeof(ifinfomsg));

                        // todo: filter any non-network interfaces (family)

                        posix_network_interface_impl nwif;

                        nwif._index = iface->ifi_index;
                        nwif._loopback = (iface->ifi_flags & IFF_LOOPBACK) != 0;
                        nwif._up = (iface->ifi_flags & IFF_UP) != 0;
    #if defined(IFF_802_1Q_VLAN) && defined(IFF_EBRIDGE) && defined(IFF_SLAVE_INACTIVE)
                        nwif._virtual = (iface->ifi_flags & (IFF_802_1Q_VLAN|IFF_EBRIDGE|IFF_SLAVE_INACTIVE)) != 0;
    #endif
                        for (auto* attribute = IFLA_RTA(iface); RTA_OK(attribute, ilen); attribute = RTA_NEXT(attribute, ilen)) {
                            switch(attribute->rta_type) {
                            case IFLA_IFNAME:
                                nwif._name = reinterpret_cast<const char *>(RTA_DATA(attribute));
                                break;
                            case IFLA_MTU:
                                nwif._mtu = *reinterpret_cast<const uint32_t *>(RTA_DATA(attribute));
                                break;
                            case IFLA_ADDRESS:
                                nwif._hardware_address.assign(reinterpret_cast<const uint8_t *>(RTA_DATA(attribute)), reinterpret_cast<const uint8_t *>(RTA_DATA(attribute)) + RTA_PAYLOAD(attribute));
                                break;
                            default:
                                break;
                            }
                        }

                        res.emplace_back(std::move(nwif));

                        break;
                    }
                    case RTM_NEWADDR:
                    {
                        auto* addr = reinterpret_cast<const ifaddrmsg*>(NLMSG_DATA(msg_ptr));
                        auto ilen = msg_ptr->nlmsg_len - NLMSG_LENGTH(sizeof(ifaddrmsg));

                        for (auto& nwif : res) {
                            if (nwif._index == addr->ifa_index) {
                                for (auto* attribute = IFA_RTA(addr); RTA_OK(attribute, ilen); attribute = RTA_NEXT(attribute, ilen)) {
                                    std::optional<inet_address> ia;

                                    switch(attribute->rta_type) {
                                    case IFA_LOCAL:
                                    case IFA_ADDRESS: // ipv6 addresses are reported only as "ADDRESS"

                                        if (RTA_PAYLOAD(attribute) == sizeof(::in_addr)) {
                                            ia.emplace(*reinterpret_cast<const ::in_addr *>(RTA_DATA(attribute)));
                                        } else if (RTA_PAYLOAD(attribute) == sizeof(::in6_addr)) {
                                            ia.emplace(*reinterpret_cast<const ::in6_addr *>(RTA_DATA(attribute)), nwif.index());
                                        }

                                        if (ia && std::find(nwif._addresses.begin(), nwif._addresses.end(), *ia) == nwif._addresses.end()) {
                                            nwif._addresses.emplace_back(*ia);
                                        }

                                        break;
                                    default:
                                        break;
                                    }
                                }

                                break;
                            }
                        }

                        break;
                    }
                    default:
                        break;
                    }
                }
            }
        }

        return res;
    }();

    // And a similarly immutable set of shared_ptr to network_interface_impl per shard, ready
    // to be handed out to callers with minimal overhead
    static const thread_local std::vector<shared_ptr<posix_network_interface_impl>> thread_local_interfaces = [] {
        std::vector<shared_ptr<posix_network_interface_impl>> res;
        res.reserve(global_interfaces.size());
        std::transform(global_interfaces.begin(), global_interfaces.end(), std::back_inserter(res), [](const posix_network_interface_impl& impl) {
            return make_shared<posix_network_interface_impl>(impl);
        });
        return res;
    }();

    return std::vector<network_interface>(thread_local_interfaces.begin(), thread_local_interfaces.end());
}

statistics posix_network_stack::stats(unsigned scheduling_group_id) {
    return statistics{
        bytes_sent[scheduling_group_id],
        bytes_received[scheduling_group_id],
    };
}

void posix_network_stack::clear_stats(unsigned scheduling_group_id) {
    bytes_sent[scheduling_group_id] = 0;
    bytes_received[scheduling_group_id] = 0;
}

}

}
