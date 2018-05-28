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

#include "stack.hh"
#include "core/reactor.hh"

namespace seastar {

net::udp_channel::udp_channel()
{}

net::udp_channel::udp_channel(std::unique_ptr<udp_channel_impl> impl) : _impl(std::move(impl))
{}

net::udp_channel::~udp_channel()
{}

net::udp_channel::udp_channel(udp_channel&&) = default;
net::udp_channel& net::udp_channel::operator=(udp_channel&&) = default;

future<net::udp_datagram> net::udp_channel::receive() {
    return _impl->receive();
}

future<> net::udp_channel::send(ipv4_addr dst, const char* msg) {
    return _impl->send(std::move(dst), msg);
}

future<> net::udp_channel::send(ipv4_addr dst, packet p) {
    return _impl->send(std::move(dst), std::move(p));
}

bool net::udp_channel::is_closed() const {
    return _impl->is_closed();
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

void socket::shutdown() {
    _si->shutdown();
}

server_socket::server_socket() {
}

server_socket::server_socket(std::unique_ptr<net::server_socket_impl> ssi)
        : _ssi(std::move(ssi)) {
}
server_socket::server_socket(server_socket&& ss) noexcept = default;
server_socket& server_socket::operator=(server_socket&& cs) noexcept = default;

server_socket::~server_socket() {
}

future<connected_socket, socket_address> server_socket::accept() {
    if (_aborted) {
        return make_exception_future<connected_socket, socket_address>(std::system_error(ECONNABORTED, std::system_category()));
    }
    return _ssi->accept();
}

void server_socket::abort_accept() {
    _ssi->abort_accept();
    _aborted = true;
}

socket_address::socket_address(ipv4_addr addr)
    : socket_address(make_ipv4_address(addr))
{}


bool socket_address::operator==(const socket_address& a) const {
    // TODO: handle ipv6
    return std::tie(u.in.sin_family, u.in.sin_port, u.in.sin_addr.s_addr)
                    == std::tie(a.u.in.sin_family, a.u.in.sin_port,
                                    a.u.in.sin_addr.s_addr);
}

}
