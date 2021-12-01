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
#pragma once

#include <chrono>
#include <seastar/net/api.hh>
#include <seastar/core/memory.hh>
#include "../core/internal/api-level.hh"

namespace seastar {

namespace net {

/// \cond internal
class connected_socket_impl {
public:
    virtual ~connected_socket_impl() {}
    virtual data_source source() = 0;
    virtual data_source source(connected_socket_input_stream_config csisc);
    virtual data_sink sink() = 0;
    virtual void shutdown_input() = 0;
    virtual void shutdown_output() = 0;
    virtual void set_nodelay(bool nodelay) = 0;
    virtual bool get_nodelay() const = 0;
    virtual void set_keepalive(bool keepalive) = 0;
    virtual bool get_keepalive() const = 0;
    virtual void set_keepalive_parameters(const keepalive_params&) = 0;
    virtual keepalive_params get_keepalive_parameters() const = 0;
    virtual void set_sockopt(int level, int optname, const void* data, size_t len) = 0;
    virtual int get_sockopt(int level, int optname, void* data, size_t len) const = 0;
    virtual socket_address local_address() const noexcept = 0;
};

class socket_impl {
public:
    virtual ~socket_impl() {}
    virtual future<connected_socket> connect(socket_address sa, socket_address local, transport proto = transport::TCP) = 0;
    virtual void set_reuseaddr(bool reuseaddr) = 0;
    virtual bool get_reuseaddr() const = 0;
    virtual void shutdown() = 0;
};


class server_socket_impl {
public:
    virtual ~server_socket_impl() {}
    virtual future<accept_result> accept() = 0;
    virtual void abort_accept() = 0;
    virtual socket_address local_address() const = 0;
};

class udp_channel_impl {
public:
    virtual ~udp_channel_impl() {}
    virtual socket_address local_address() const = 0;
    virtual future<udp_datagram> receive() = 0;
    virtual future<> send(const socket_address& dst, const char* msg) = 0;
    virtual future<> send(const socket_address& dst, packet p) = 0;
    virtual void shutdown_input() = 0;
    virtual void shutdown_output() = 0;
    virtual bool is_closed() const = 0;
    virtual void close() = 0;
};

class network_interface_impl {
public:
    virtual ~network_interface_impl() {}
    virtual uint32_t index() const = 0;
    virtual uint32_t mtu() const = 0;

    virtual const sstring& name() const = 0;
    virtual const sstring& display_name() const = 0;
    virtual const std::vector<net::inet_address>& addresses() const = 0;
    virtual const std::vector<uint8_t> hardware_address() const = 0;

    virtual bool is_loopback() const = 0;
    virtual bool is_virtual() const = 0;
    virtual bool is_up() const = 0;
    virtual bool supports_ipv6() const = 0;
};

/// \endcond

}

}
