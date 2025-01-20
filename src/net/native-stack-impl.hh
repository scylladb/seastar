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

#include <seastar/net/stack.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/log.hh>

namespace seastar {

extern logger seastar_logger;

namespace internal {

namespace native_stack_net_stats {

inline thread_local std::array<uint64_t, max_scheduling_groups()> bytes_sent = {};
inline thread_local std::array<uint64_t, max_scheduling_groups()> bytes_received = {};

};

}

namespace net {

using namespace seastar;

template <typename Protocol>
class native_server_socket_impl;

template <typename Protocol>
class native_connected_socket_impl;

class native_network_stack;

// native_server_socket_impl
template <typename Protocol>
class native_server_socket_impl : public server_socket_impl {
    typename Protocol::listener _listener;
public:
    native_server_socket_impl(Protocol& proto, uint16_t port, listen_options opt);
    virtual future<accept_result> accept() override;
    virtual void abort_accept() override;
    virtual socket_address local_address() const override;
};

template <typename Protocol>
native_server_socket_impl<Protocol>::native_server_socket_impl(Protocol& proto, uint16_t port, listen_options opt)
    : _listener(proto.listen(port)) {
}

template <typename Protocol>
future<accept_result>
native_server_socket_impl<Protocol>::accept() {
    return _listener.accept().then([] (typename Protocol::connection conn) {
        // Save "conn" contents before call below function
        // "conn" is moved in 1st argument, and used in 2nd argument
        // It causes trouble on Arm which passes arguments from left to right
        auto ip = conn.foreign_ip().ip;
        auto port = conn.foreign_port();
        return make_ready_future<accept_result>(accept_result{
                connected_socket(std::make_unique<native_connected_socket_impl<Protocol>>(make_lw_shared(std::move(conn)))),
                make_ipv4_address(ip, port)});
    });
}

template <typename Protocol>
void
native_server_socket_impl<Protocol>::abort_accept() {
    _listener.abort_accept();
}

template <typename Protocol>
socket_address native_server_socket_impl<Protocol>::local_address() const {
    return socket_address(_listener.get_tcp().inet().inet().host_address(), _listener.port());
}

// native_connected_socket_impl
template <typename Protocol>
class native_connected_socket_impl : public connected_socket_impl {
    lw_shared_ptr<typename Protocol::connection> _conn;
    class native_data_source_impl;
    class native_data_sink_impl;
public:
    explicit native_connected_socket_impl(lw_shared_ptr<typename Protocol::connection> conn)
        : _conn(std::move(conn)) {}
    using connected_socket_impl::source;
    virtual data_source source() override;
    virtual data_sink sink() override;
    virtual void shutdown_input() override;
    virtual void shutdown_output() override;
    virtual void set_nodelay(bool nodelay) override;
    virtual bool get_nodelay() const override;
    void set_keepalive(bool keepalive) override;
    bool get_keepalive() const override;
    void set_keepalive_parameters(const keepalive_params&) override;
    keepalive_params get_keepalive_parameters() const override;
    int get_sockopt(int level, int optname, void* data, size_t len) const override;
    void set_sockopt(int level, int optname, const void* data, size_t len) override;
    socket_address local_address() const noexcept override;
    socket_address remote_address() const noexcept override;
    virtual future<> wait_input_shutdown() override;
};

template <typename Protocol>
class native_socket_impl final : public socket_impl {
    Protocol& _proto;
    lw_shared_ptr<typename Protocol::connection> _conn;
public:
    explicit native_socket_impl(Protocol& proto)
        : _proto(proto), _conn(nullptr) { }

    virtual future<connected_socket> connect(socket_address sa, socket_address local, transport proto = transport::TCP) override {
        //TODO: implement SCTP
        SEASTAR_ASSERT(proto == transport::TCP);

        // FIXME: local is ignored since native stack does not support multiple IPs yet
        SEASTAR_ASSERT(sa.as_posix_sockaddr().sa_family == AF_INET);

        _conn = make_lw_shared<typename Protocol::connection>(_proto.connect(sa));
        return _conn->connected().then([conn = _conn]() mutable {
            auto csi = std::make_unique<native_connected_socket_impl<Protocol>>(std::move(conn));
            return make_ready_future<connected_socket>(connected_socket(std::move(csi)));
        });
    }

    virtual void set_reuseaddr(bool reuseaddr) override {
        // FIXME: implement
        seastar_logger.error("Reuseaddr is not supported by native stack");
    }

    virtual bool get_reuseaddr() const override {
        // FIXME: implement
        return false;
    }

    virtual void shutdown() override {
        if (_conn) {
            _conn->shutdown_connect();
        }
    }
};

template <typename Protocol>
class native_connected_socket_impl<Protocol>::native_data_source_impl final
    : public data_source_impl {
    typedef typename Protocol::connection connection_type;
    lw_shared_ptr<connection_type> _conn;
    size_t _cur_frag = 0;
    bool _eof = false;
    packet _buf;
public:
    explicit native_data_source_impl(lw_shared_ptr<connection_type> conn)
        : _conn(std::move(conn)) {}
    virtual future<temporary_buffer<char>> get() override {
        if (_eof) {
            return make_ready_future<temporary_buffer<char>>(temporary_buffer<char>(0));
        }
        if (_cur_frag != _buf.nr_frags()) {
            auto& f = _buf.fragments()[_cur_frag++];
            return make_ready_future<temporary_buffer<char>>(
                    temporary_buffer<char>(f.base, f.size,
                            make_deleter(deleter(), [p = _buf.share()] () mutable {})));
        }
        return _conn->wait_for_data().then([this] {
            _buf = _conn->read();
            auto sg_id = internal::scheduling_group_index(current_scheduling_group());
            internal::native_stack_net_stats::bytes_received[sg_id] += _buf.len();
            _cur_frag = 0;
            _eof = !_buf.len();
            return get();
        });
    }
    future<> close() override {
        _conn->close_write();
        return make_ready_future<>();
    }
};

template <typename Protocol>
class native_connected_socket_impl<Protocol>::native_data_sink_impl final
    : public data_sink_impl {
    typedef typename Protocol::connection connection_type;
    lw_shared_ptr<connection_type> _conn;
public:
    explicit native_data_sink_impl(lw_shared_ptr<connection_type> conn)
        : _conn(std::move(conn)) {}
    using data_sink_impl::put;
    virtual future<> put(packet p) override {
        auto sg_id = internal::scheduling_group_index(current_scheduling_group());
        internal::native_stack_net_stats::bytes_sent[sg_id] += p.len();
        return _conn->send(std::move(p));
    }
    virtual future<> close() override {
        _conn->close_write();
        return make_ready_future<>();
    }
    virtual bool can_batch_flushes() const noexcept override { return true; }
    virtual void on_batch_flush_error() noexcept override {
        _conn->close_read();
    }
};

template <typename Protocol>
data_source native_connected_socket_impl<Protocol>::source() {
    return data_source(std::make_unique<native_data_source_impl>(_conn));
}

template <typename Protocol>
data_sink native_connected_socket_impl<Protocol>::sink() {
    return data_sink(std::make_unique<native_data_sink_impl>(_conn));
}

template <typename Protocol>
void
native_connected_socket_impl<Protocol>::shutdown_input() {
    _conn->close_read();
}

template <typename Protocol>
void
native_connected_socket_impl<Protocol>::shutdown_output() {
    _conn->close_write();
}

template <typename Protocol>
void
native_connected_socket_impl<Protocol>::set_nodelay(bool nodelay) {
    // FIXME: implement
}

template <typename Protocol>
bool
native_connected_socket_impl<Protocol>::get_nodelay() const {
    // FIXME: implement
    return true;
}

template <typename Protocol>
void native_connected_socket_impl<Protocol>::set_keepalive(bool keepalive) {
    // FIXME: implement
    seastar_logger.error("Keepalive is not supported by native stack");
}
template <typename Protocol>
bool native_connected_socket_impl<Protocol>::get_keepalive() const {
    // FIXME: implement
    return false;
}

template <typename Protocol>
void native_connected_socket_impl<Protocol>::set_keepalive_parameters(const keepalive_params&) {
    // FIXME: implement
    seastar_logger.error("Keepalive parameters are not supported by native stack");
}

template <typename Protocol>
keepalive_params native_connected_socket_impl<Protocol>::get_keepalive_parameters() const {
    // FIXME: implement
    return tcp_keepalive_params {std::chrono::seconds(0), std::chrono::seconds(0), 0};
}

template<typename Protocol>
void native_connected_socket_impl<Protocol>::set_sockopt(int level, int optname, const void* data, size_t len) {
    throw std::runtime_error("Setting custom socket options is not supported for native stack");
}

template<typename Protocol>
int native_connected_socket_impl<Protocol>::get_sockopt(int level, int optname, void* data, size_t len) const {
    throw std::runtime_error("Getting custom socket options is not supported for native stack");
}

template<typename Protocol>
socket_address native_connected_socket_impl<Protocol>::local_address() const noexcept {
    return {_conn->local_ip(), _conn->local_port()};
}

template<typename Protocol>
socket_address native_connected_socket_impl<Protocol>::remote_address() const noexcept {
    return {_conn->foreign_ip(), _conn->foreign_port()};
}

template <typename Protocol>
future<> native_connected_socket_impl<Protocol>::wait_input_shutdown() {
    return _conn->wait_input_shutdown();
}

}

}
