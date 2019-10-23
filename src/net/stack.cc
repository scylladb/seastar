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
 * Copyright 2015 Cloudius Systems
 */

#include <seastar/net/stack.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/core/reactor.hh>

namespace seastar {

net::udp_channel::udp_channel()
{}

net::udp_channel::udp_channel(std::unique_ptr<udp_channel_impl> impl) : _impl(std::move(impl))
{}

net::udp_channel::~udp_channel()
{}

net::udp_channel::udp_channel(udp_channel&&) = default;
net::udp_channel& net::udp_channel::operator=(udp_channel&&) = default;

socket_address net::udp_channel::local_address() const {
    return _impl->local_address();
}

future<net::udp_datagram> net::udp_channel::receive() {
    return _impl->receive();
}

future<> net::udp_channel::send(const socket_address& dst, const char* msg) {
    return _impl->send(dst, msg);
}

future<> net::udp_channel::send(const socket_address& dst, packet p) {
    return _impl->send(dst, std::move(p));
}

bool net::udp_channel::is_closed() const {
    return _impl->is_closed();
}

void net::udp_channel::shutdown_input() {
    _impl->shutdown_input();
}

void net::udp_channel::shutdown_output() {
    _impl->shutdown_output();
}


void net::udp_channel::close() {
    return _impl->close();
}

connected_socket::connected_socket()
{}

connected_socket::connected_socket(
        std::unique_ptr<net::connected_socket_impl> csi)
        : _csi(std::move(csi)) {
}

connected_socket::connected_socket(connected_socket&& cs) noexcept = default;
connected_socket& connected_socket::operator=(connected_socket&& cs) noexcept = default;

connected_socket::~connected_socket()
{}

input_stream<char> connected_socket::input() {
    return input_stream<char>(_csi->source());
}

output_stream<char> connected_socket::output(size_t buffer_size) {
    // TODO: allow user to determine buffer size etc
    return output_stream<char>(_csi->sink(), buffer_size, false, true);
}

void connected_socket::set_nodelay(bool nodelay) {
    _csi->set_nodelay(nodelay);
}

bool connected_socket::get_nodelay() const {
    return _csi->get_nodelay();
}
void connected_socket::set_keepalive(bool keepalive) {
    _csi->set_keepalive(keepalive);
}
bool connected_socket::get_keepalive() const {
    return _csi->get_keepalive();
}
void connected_socket::set_keepalive_parameters(const net::keepalive_params& p) {
    _csi->set_keepalive_parameters(p);
}
net::keepalive_params connected_socket::get_keepalive_parameters() const {
    return _csi->get_keepalive_parameters();
}

void connected_socket::shutdown_output() {
    _csi->shutdown_output();
}

void connected_socket::shutdown_input() {
    _csi->shutdown_input();
}

socket::~socket()
{}

socket::socket(
        std::unique_ptr<net::socket_impl> si)
        : _si(std::move(si)) {
}

socket::socket(socket&&) noexcept = default;
socket& socket::operator=(socket&&) noexcept = default;

future<connected_socket> socket::connect(socket_address sa, socket_address local, transport proto) {
    return _si->connect(sa, local, proto);
}

void socket::set_reuseaddr(bool reuseaddr) {
    _si->set_reuseaddr(reuseaddr);
}

bool socket::get_reuseaddr() const {
    return _si->get_reuseaddr();
}

void socket::shutdown() {
    _si->shutdown();
}

#if SEASTAR_API_LEVEL <= 1

namespace internal {

class api_v1_to_v2_server_socket_impl_adapter : public net::api_v2::server_socket_impl {
    std::unique_ptr<net::api_v1::server_socket_impl> _v1;
public:
    explicit api_v1_to_v2_server_socket_impl_adapter(std::unique_ptr<net::api_v1::server_socket_impl> v1)
            : _v1(std::move(v1)) {
    }
    virtual future<accept_result> accept() override {
        return _v1->accept().then([] (connected_socket cs, socket_address sa) {
            return accept_result{std::move(cs), std::move(sa)};
        });
    }
    virtual void abort_accept() override {
        return _v1->abort_accept();
    }
    virtual socket_address local_address() const override {
        return _v1->local_address();
    }
};

}

#endif

SEASTAR_INCLUDE_API_V2 namespace api_v2 {

server_socket::server_socket() {
}

server_socket::server_socket(std::unique_ptr<net::api_v2::server_socket_impl> ssi)
        : _ssi(std::move(ssi)) {
}
server_socket::server_socket(server_socket&& ss) noexcept = default;
server_socket& server_socket::operator=(server_socket&& cs) noexcept = default;

server_socket::~server_socket() {
}

future<accept_result> server_socket::accept() {
    if (_aborted) {
        return make_exception_future<accept_result>(std::system_error(ECONNABORTED, std::system_category()));
    }
    return _ssi->accept();
}

void server_socket::abort_accept() {
    _ssi->abort_accept();
    _aborted = true;
}

socket_address server_socket::local_address() const {
    return _ssi->local_address();
}

}

#if SEASTAR_API_LEVEL <= 1

SEASTAR_INCLUDE_API_V1 namespace api_v1 {

server_socket::server_socket() {
}

api_v2::server_socket
server_socket::make_v2_server_socket(std::unique_ptr<net::api_v1::server_socket_impl> ssi_v1) {
    auto ssi_v2 = std::make_unique<internal::api_v1_to_v2_server_socket_impl_adapter>(std::move(ssi_v1));
    return api_v2::server_socket(std::move(ssi_v2));
}

server_socket::server_socket(std::unique_ptr<net::api_v1::server_socket_impl> ssi)
        : _impl(make_v2_server_socket(std::move(ssi))) {
}

server_socket::server_socket(std::unique_ptr<net::api_v2::server_socket_impl> ssi)
        : _impl(api_v2::server_socket(std::move(ssi))) {
}

server_socket::server_socket(server_socket&& ss) noexcept = default;

server_socket::server_socket(api_v2::server_socket&& ss)
        : _impl(std::move(ss)) {
}

server_socket& server_socket::operator=(server_socket&& cs) noexcept = default;

server_socket::~server_socket() {
}

server_socket::operator api_v2::server_socket() && {
    return std::move(_impl);
}

future<connected_socket, socket_address> server_socket::accept() {
    return _impl.accept().then([] (accept_result ar) {
        return make_ready_future<connected_socket, socket_address>(std::move(ar.connection), std::move(ar.remote_address));
    });
}


void server_socket::abort_accept() {
    return _impl.abort_accept();
}

socket_address server_socket::local_address() const {
    return _impl.local_address();
}

}


#endif

socket_address::socket_address() 
    // set max addr_length, as we (probably) want to use the constructed object
    // in accept() or get_address()
    : addr_length(sizeof(::sockaddr_storage))
{
    static_assert(AF_UNSPEC == 0, "just checking");
    memset(&u, 0, sizeof(u));
}

socket_address::socket_address(uint16_t p)
    : socket_address(ipv4_addr(p))
{}

socket_address::socket_address(ipv4_addr addr)
{
    addr_length = sizeof(::sockaddr_in);
    u.in.sin_family = AF_INET;
    u.in.sin_port = htons(addr.port);
    u.in.sin_addr.s_addr = htonl(addr.ip);
}

socket_address::socket_address(const ipv6_addr& addr, uint32_t scope)
{
    addr_length = sizeof(::sockaddr_in6);
    u.in6.sin6_family = AF_INET6;
    u.in6.sin6_port = htons(addr.port);
    u.in6.sin6_flowinfo = 0;
    u.in6.sin6_scope_id = scope;
    std::copy(addr.ip.begin(), addr.ip.end(), u.in6.sin6_addr.s6_addr);
}

socket_address::socket_address(const ipv6_addr& addr)
    : socket_address(addr, net::inet_address::invalid_scope)
{}

socket_address::socket_address(uint32_t ipv4, uint16_t p)
    : socket_address(make_ipv4_address(ipv4, p))
{}

bool socket_address::is_unspecified() const {
    return u.sa.sa_family == AF_UNSPEC;
}

static int adjusted_path_length(const socket_address& a) {
    int l = std::max(0, (int)a.addr_length-(int)((size_t) (((struct sockaddr_un *) 0)->sun_path)));
    // "un-count" a trailing null in filesystem-namespace paths
    if (a.u.un.sun_path[0]!='\0' && (l > 1) && a.u.un.sun_path[l-1]=='\0') {
        --l;
    }
    return l;
}

bool socket_address::operator==(const socket_address& a) const {
    if (u.sa.sa_family != a.u.sa.sa_family) {
        return false;
    }
    if (u.sa.sa_family == AF_UNIX) {
        // tolerate counting/not counting a terminating null in filesystem-namespace paths
        int adjusted_len = adjusted_path_length(*this);
        int a_adjusted_len = adjusted_path_length(a);
        if (adjusted_len != a_adjusted_len) {
            return false;
        }
        return (memcmp(u.un.sun_path, a.u.un.sun_path, adjusted_len) == 0);
    }

    // an INET address
    if (u.in.sin_port != a.u.in.sin_port) {
        return false;
    }
    switch (u.sa.sa_family) {
    case AF_INET:
        return u.in.sin_addr.s_addr == a.u.in.sin_addr.s_addr;
    case AF_UNSPEC:
    case AF_INET6:
        // handled below
        break;
    default:
        return false;
    }

    auto& in1 = as_posix_sockaddr_in6();
    auto& in2 = a.as_posix_sockaddr_in6();

    return IN6_ARE_ADDR_EQUAL(&in1, &in2);
}

network_interface::network_interface(shared_ptr<net::network_interface_impl> impl)
    : _impl(std::move(impl))
{}

network_interface::network_interface(network_interface&&) = default;
network_interface& network_interface::operator=(network_interface&&) = default;
    
uint32_t network_interface::index() const {
    return _impl->index();
}

uint32_t network_interface::mtu() const {
    return _impl->mtu();
}

const sstring& network_interface::name() const {
    return _impl->name();
}

const sstring& network_interface::display_name() const {
    return _impl->display_name();
}

const std::vector<net::inet_address>& network_interface::addresses() const {
    return _impl->addresses();
}

const std::vector<uint8_t> network_interface::hardware_address() const {
    return _impl->hardware_address();
}

bool network_interface::is_loopback() const {
    return _impl->is_loopback();
}

bool network_interface::is_virtual() const {
    return _impl->is_virtual();
}

bool network_interface::is_up() const {
    return _impl->is_up();
}

bool network_interface::supports_ipv6() const {
    return _impl->supports_ipv6();
}


future<connected_socket>
network_stack::connect(socket_address sa, socket_address local, transport proto) {
    return do_with(socket(), [sa, local, proto](::seastar::socket& s) {
        return s.connect(sa, local, proto);
    });
}

std::vector<network_interface> network_stack::network_interfaces() {
    return {};
}

}
