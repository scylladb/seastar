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
#include <seastar/core/on_internal_error.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/log.hh>

namespace seastar {

extern logger seastar_logger;

static_assert(std::is_nothrow_default_constructible_v<connected_socket>);
static_assert(std::is_nothrow_move_constructible_v<connected_socket>);

static_assert(std::is_nothrow_default_constructible_v<socket>);
static_assert(std::is_nothrow_move_constructible_v<socket>);

static_assert(std::is_nothrow_default_constructible_v<server_socket>);
static_assert(std::is_nothrow_move_constructible_v<server_socket>);

net::udp_channel::udp_channel() noexcept
{}

net::udp_channel::udp_channel(std::unique_ptr<udp_channel_impl> impl) noexcept : _impl(std::move(impl))
{}

net::udp_channel::~udp_channel()
{}

net::udp_channel::udp_channel(udp_channel&& uc) noexcept = default;

net::udp_channel& net::udp_channel::operator=(udp_channel&& uc) noexcept = default;

socket_address net::udp_channel::local_address() const {
    if (_impl) {
        return _impl->local_address();
    } else {
        return {};
    }
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
    if (_impl) {
        _impl->close();
        _impl.reset();
    }
}

connected_socket::connected_socket() noexcept
{
    seastar_logger.debug("constructed connected_socket {} with impl {}, at {}", fmt::ptr(this), fmt::ptr(_csi.get()), current_backtrace());
}

connected_socket::connected_socket(
        std::unique_ptr<net::connected_socket_impl> csi) noexcept
        : _csi(std::move(csi)) {
    seastar_logger.debug("constructed connected_socket {} with impl {}, at {}", fmt::ptr(this), fmt::ptr(_csi.get()), current_backtrace());
}

connected_socket::connected_socket(connected_socket&& cs) noexcept
        : _csi(std::move(cs._csi))
        , _socket(std::move(cs._socket))
{
    seastar_logger.debug("move-constructed connected_socket {} with impl {} and socket_impl {} from {}, at {}", fmt::ptr(this), fmt::ptr(_csi.get()), fmt::ptr(_socket.impl()), fmt::ptr(&cs), current_backtrace());
}
connected_socket& connected_socket::operator=(connected_socket&& cs) noexcept {
    if (this != &cs) {
        _csi = std::move(cs._csi);
        assert(!_socket.impl());
        _socket = std::move(cs._socket);
        seastar_logger.debug("move-assigned connected_socket {} with impl {} and socket_impl {} from {}, at {}", fmt::ptr(this), fmt::ptr(_csi.get()), fmt::ptr(_socket.impl()), fmt::ptr(&cs), current_backtrace());
    }
    return *this;
}

connected_socket::~connected_socket()
{
    seastar_logger.debug("destroying connected_socket {} with impl {} and socket_impl {}, at {}", fmt::ptr(this), fmt::ptr(_csi.get()), fmt::ptr(_socket.impl()), current_backtrace());
#if SEASTAR_API_LEVEL >= 7
    if (_csi || _socket.impl()) {
        on_internal_error_noexcept(seastar_logger, "connected_socket destroyed while open");
    }
#endif
}

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

void connected_socket::shutdown_output() {
    _socket.shutdown();
    if (_csi) {
        _csi->shutdown_output();
    }
}

void connected_socket::shutdown_input() {
    _socket.shutdown();
    if (_csi) {
        _csi->shutdown_input();
    }
}

future<> connected_socket::close() noexcept {
    seastar_logger.debug("closing connected_socket {} with impl {}, at {}", fmt::ptr(this), fmt::ptr(_csi.get()), current_backtrace());
    auto close_csi = _csi ? _csi->close().then([csi = std::move(_csi)] {}) : make_ready_future<>();
    auto close_sock = _socket.impl() ? _socket.close().then([s = std::move(_socket)] {}) : make_ready_future<>();
    return when_all(std::move(close_csi), std::move(close_sock)).discard_result();
}

void connected_socket::set_socket(socket&& s) noexcept {
    seastar_logger.debug("connected_socket {} set_socket {}, at {}", fmt::ptr(this), fmt::ptr(&s), current_backtrace());
    _socket = std::move(s);
}

data_source
net::connected_socket_impl::source(connected_socket_input_stream_config csisc) {
    // Default implementation falls back to non-parameterized data_source
    return source();
}

socket::~socket()
{
    seastar_logger.debug("destroying socket {} with impl {}, at {}", fmt::ptr(this), fmt::ptr(_si.get()), current_backtrace());
#if SEASTAR_API_LEVEL >= 7
    if (_si) {
        on_internal_error_noexcept(seastar_logger, "socket destroyed while open");
    }
#endif
}

socket::socket(
        std::unique_ptr<net::socket_impl> si) noexcept
        : _si(std::move(si)) {
    seastar_logger.debug("constructed socket {} with impl {}, at {}", fmt::ptr(this), fmt::ptr(_si.get()), current_backtrace());
}

socket::socket(socket&& s) noexcept
    : _si(std::move(s._si))
{
    seastar_logger.debug("move-constructed socket {} with impl {} from {}, at {}", fmt::ptr(this), fmt::ptr(_si.get()), fmt::ptr(&s), current_backtrace());
}

socket& socket::operator=(socket&& s) noexcept {
    if (this != &s) {
        _si = std::move(s._si);
        seastar_logger.debug("move-assigned socket {} with impl {} from {}, at {}", fmt::ptr(this), fmt::ptr(_si.get()), fmt::ptr(&s), current_backtrace());
    }
    return *this;
}

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
  if (_si) {
    _si->shutdown();
  }
}

future<> socket::close() noexcept {
    seastar_logger.debug("closing socket {} with impl {}, at {}", fmt::ptr(this), fmt::ptr(_si.get()), current_backtrace());
    return _si ? _si->close().then([si = std::move(_si)] {}) : make_ready_future<>();
}

const net::socket_impl* socket::impl() const noexcept {
    return _si.get();
}

server_socket::server_socket() noexcept {
    seastar_logger.debug("constructed server_socket {} with impl {}, at {}", fmt::ptr(this), fmt::ptr(_ssi.get()), current_backtrace());
}

server_socket::server_socket(std::unique_ptr<net::server_socket_impl> ssi) noexcept
        : _ssi(std::move(ssi)) {
    seastar_logger.debug("constructed server_socket {} with impl {}, at {}", fmt::ptr(this), fmt::ptr(_ssi.get()), current_backtrace());
}
server_socket::server_socket(server_socket&& ss) noexcept
        : _ssi(std::move(ss._ssi))
        , _aborted(ss._aborted)
{
    seastar_logger.debug("move-constructed server_socket {} with impl {} from {}, at {}", fmt::ptr(this), fmt::ptr(_ssi.get()), fmt::ptr(&ss), current_backtrace());
}
server_socket& server_socket::operator=(server_socket&& cs) noexcept = default;

server_socket::~server_socket() {
    seastar_logger.debug("destroying server_socket {} with impl {}, at {}", fmt::ptr(this), fmt::ptr(_ssi.get()), current_backtrace());
#if SEASTAR_API_LEVEL >= 7
    if (_ssi) {
        on_internal_error_noexcept(seastar_logger, "server_socket destroyed while open");
    }
#endif
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

future<> server_socket::close() noexcept {
    seastar_logger.debug("closing server_socket {} with impl {}, at {}", fmt::ptr(this), fmt::ptr(_ssi.get()), current_backtrace());
    if (!_ssi) {
        return make_ready_future<>();
    }
    return _ssi->close().then([ssi = std::move(_ssi)] {});
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
        return s.connect(sa, local, proto).then_wrapped([&s] (future<connected_socket> f) {
            if (f.failed()) {
                auto ex = f.get_exception();
                return s.close().then([s = std::move(s), ex = std::move(ex)] () mutable {
                    return make_exception_future<connected_socket>(std::move(ex));
                });
            }
            auto cs = f.get0();
            cs.set_socket(std::move(s));
            return make_ready_future<connected_socket>(std::move(cs));
        });
    });
}

std::vector<network_interface> network_stack::network_interfaces() {
    return {};
}

}
