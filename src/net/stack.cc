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

#ifdef SEASTAR_MODULE
module;
#endif

#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/metrics_api.hh>
#include <seastar/core/reactor.hh>
#include <seastar/net/stack.hh>
#include <seastar/net/inet_address.hh>
#endif

namespace seastar {

static_assert(std::is_nothrow_default_constructible_v<connected_socket>);
static_assert(std::is_nothrow_move_constructible_v<connected_socket>);

static_assert(std::is_nothrow_default_constructible_v<socket>);
static_assert(std::is_nothrow_move_constructible_v<socket>);

static_assert(std::is_nothrow_default_constructible_v<server_socket>);
static_assert(std::is_nothrow_move_constructible_v<server_socket>);

net::datagram_channel::datagram_channel() noexcept
{}

net::datagram_channel::datagram_channel(std::unique_ptr<datagram_channel_impl> impl) noexcept : _impl(std::move(impl))
{}

net::datagram_channel::~datagram_channel()
{}

net::datagram_channel::datagram_channel(datagram_channel&&) noexcept = default;
net::datagram_channel& net::datagram_channel::operator=(datagram_channel&&) noexcept = default;

socket_address net::datagram_channel::local_address() const {
    if (_impl) {
        return _impl->local_address();
    } else {
        return {};
    }
}

future<net::datagram> net::datagram_channel::receive() {
    return _impl->receive();
}

future<> net::datagram_channel::send(const socket_address& dst, const char* msg) {
    return _impl->send(dst, msg);
}

future<> net::datagram_channel::send(const socket_address& dst, packet p) {
    return _impl->send(dst, std::move(p));
}

bool net::datagram_channel::is_closed() const {
    return _impl->is_closed();
}

void net::datagram_channel::shutdown_input() {
    _impl->shutdown_input();
}

void net::datagram_channel::shutdown_output() {
    _impl->shutdown_output();
}


void net::datagram_channel::close() {
    return _impl->close();
}

connected_socket::connected_socket() noexcept
{}

connected_socket::connected_socket(
        std::unique_ptr<net::connected_socket_impl> csi) noexcept
        : _csi(std::move(csi)) {
}

connected_socket::connected_socket(connected_socket&& cs) noexcept = default;
connected_socket& connected_socket::operator=(connected_socket&& cs) noexcept = default;

connected_socket::~connected_socket()
{}

input_stream<char> connected_socket::input(connected_socket_input_stream_config csisc) {
    return input_stream<char>(_csi->source(csisc));
}

output_stream<char> connected_socket::output(size_t buffer_size) {
    output_stream_options opts;
    opts.batch_flushes = true;
    // TODO: allow user to determine buffer size etc
    return output_stream<char>(_csi->sink(), buffer_size, opts);
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
void connected_socket::set_sockopt(int level, int optname, const void* data, size_t len) {
    _csi->set_sockopt(level, optname, data, len);
}
int connected_socket::get_sockopt(int level, int optname, void* data, size_t len) const {
    return _csi->get_sockopt(level, optname, data, len);
}

socket_address connected_socket::local_address() const noexcept {
    return _csi->local_address();
}

socket_address connected_socket::remote_address() const noexcept {
    return _csi->remote_address();
}

void connected_socket::shutdown_output() {
    _csi->shutdown_output();
}

void connected_socket::shutdown_input() {
    _csi->shutdown_input();
}

future<> connected_socket::wait_input_shutdown() {
    return _csi->wait_input_shutdown();
}

data_source
net::connected_socket_impl::source(connected_socket_input_stream_config csisc) {
    // Default implementation falls back to non-parameterized data_source
    return source();
}

socket::~socket()
{}

socket::socket(
        std::unique_ptr<net::socket_impl> si) noexcept
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

server_socket::server_socket() noexcept {
}

server_socket::server_socket(std::unique_ptr<net::server_socket_impl> ssi) noexcept
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

socket_address server_socket::local_address() const noexcept {
    return _ssi->local_address();
}

network_interface::network_interface(shared_ptr<net::network_interface_impl> impl) noexcept
    : _impl(std::move(impl))
{}

network_interface::network_interface(network_interface&&) noexcept = default;
network_interface& network_interface::operator=(network_interface&&) noexcept = default;

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

void register_net_metrics_for_scheduling_group(
    metrics::metric_groups &metrics, unsigned sg_id, const metrics::label_instance& name) {
    namespace sm = seastar::metrics;
    metrics.add_group("network", {
        sm::make_counter("bytes_sent", [sg_id] { return engine().net().stats(sg_id).bytes_sent; },
                sm::description("Counts the number of bytes written to network sockets."), {name}),
        sm::make_counter("bytes_received", [sg_id] { return engine().net().stats(sg_id).bytes_received; },
                sm::description("Counts the number of bytes received from network sockets."), {name}),
    });

    // need to clear stats in case we recreated a SG with the same id
    // but avoid during reactor startup
    if (engine_is_ready()) {
        engine().net().clear_stats(sg_id);
    }
}

}
