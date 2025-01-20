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
#include <unordered_set>
#endif
#include <seastar/core/sharded.hh>
#include <seastar/core/internal/pollable_fd.hh>
#include <seastar/net/stack.hh>
#include <seastar/core/polymorphic_temporary_buffer.hh>
#include <seastar/core/internal/buffer_allocator.hh>
#include <seastar/util/program-options.hh>

#include <unordered_set>

namespace seastar {

namespace net {

using namespace seastar;

// We can't keep this in any of the socket servers as instance members, because a connection can
// outlive the socket server. To avoid having the whole socket_server tracked as a shared pointer,
// we will have a conntrack structure.
//
// Right now this class is used by the posix_server_socket_impl, but it could be used by any other.
class conntrack {
    class load_balancer {
        std::vector<unsigned> _cpu_load;
    public:
        load_balancer() : _cpu_load(size_t(smp::count), 0) {}
        void closed_cpu(shard_id cpu) {
            _cpu_load[cpu]--;
        }
        shard_id next_cpu() {
            // FIXME: The naive algorithm will just round robin the connections around the shards.
            // A more complex version can keep track of the amount of activity in each connection,
            // and use that information.
            auto min_el = std::min_element(_cpu_load.begin(), _cpu_load.end());
            auto cpu = shard_id(std::distance(_cpu_load.begin(), min_el));
            _cpu_load[cpu]++;
            return cpu;
        }
        shard_id force_cpu(shard_id cpu) {
            _cpu_load[cpu]++;
            return cpu;
        }
    };

    lw_shared_ptr<load_balancer> _lb;
    void closed_cpu(shard_id cpu) {
        _lb->closed_cpu(cpu);
    }
public:
    class handle {
        shard_id _host_cpu;
        shard_id _target_cpu;
        foreign_ptr<lw_shared_ptr<load_balancer>> _lb;
    public:
        handle() : _lb(nullptr) {}
        handle(shard_id cpu, lw_shared_ptr<load_balancer> lb)
            : _host_cpu(this_shard_id())
            , _target_cpu(cpu)
            , _lb(make_foreign(std::move(lb))) {}

        handle(const handle&) = delete;
        handle(handle&&) = default;
        ~handle() {
            if (!_lb) {
                return;
            }
            // FIXME: future is discarded
            (void)smp::submit_to(_host_cpu, [cpu = _target_cpu, lb = std::move(_lb)] {
                lb->closed_cpu(cpu);
            });
        }
        shard_id cpu() {
            return _target_cpu;
        }
    };
    friend class handle;

    conntrack() : _lb(make_lw_shared<load_balancer>()) {}
    handle get_handle() {
        return handle(_lb->next_cpu(), _lb);
    }
    handle get_handle(shard_id cpu) {
        return handle(_lb->force_cpu(cpu), _lb);
    }
};

class posix_data_source_impl final : public data_source_impl, private internal::buffer_allocator {
    std::pmr::polymorphic_allocator<char>* _buffer_allocator;
    pollable_fd _fd;
    connected_socket_input_stream_config _config;
private:
    virtual temporary_buffer<char> allocate_buffer() override;
public:
    explicit posix_data_source_impl(pollable_fd fd, connected_socket_input_stream_config config,
            std::pmr::polymorphic_allocator<char>* allocator=memory::malloc_allocator)
            : _buffer_allocator(allocator), _fd(std::move(fd)), _config(config) {
    }
    future<temporary_buffer<char>> get() override;
    future<> close() override;
};

class posix_data_sink_impl : public data_sink_impl {
    pollable_fd _fd;
    packet _p;
public:
    explicit posix_data_sink_impl(pollable_fd fd) : _fd(std::move(fd)) {}
    using data_sink_impl::put;
    future<> put(packet p) override;
    future<> put(temporary_buffer<char> buf) override;
    future<> close() override;
    bool can_batch_flushes() const noexcept override { return true; }
    void on_batch_flush_error() noexcept override;
};

class posix_ap_server_socket_impl : public server_socket_impl {
    using protocol_and_socket_address = std::tuple<int, socket_address>;
    struct connection {
        pollable_fd fd;
        socket_address addr;
        conntrack::handle connection_tracking_handle;
        connection(pollable_fd xfd, socket_address xaddr, conntrack::handle cth) : fd(std::move(xfd)), addr(xaddr), connection_tracking_handle(std::move(cth)) {}
    };
    using port_map_t = std::unordered_set<protocol_and_socket_address>;
    using sockets_map_t = std::unordered_map<protocol_and_socket_address, promise<accept_result>>;
    using conn_map_t = std::unordered_multimap<protocol_and_socket_address, connection>;
    static thread_local port_map_t ports;
    static thread_local sockets_map_t sockets;
    static thread_local conn_map_t conn_q;
    int _protocol;
    socket_address _sa;
    std::pmr::polymorphic_allocator<char>* _allocator;
public:
    explicit posix_ap_server_socket_impl(int protocol, socket_address sa, std::pmr::polymorphic_allocator<char>* allocator = memory::malloc_allocator);
    ~posix_ap_server_socket_impl();
    virtual future<accept_result> accept() override;
    virtual void abort_accept() override;
    socket_address local_address() const override {
        return _sa;
    }
    static void move_connected_socket(int protocol, socket_address sa, pollable_fd fd, socket_address addr, conntrack::handle handle, std::pmr::polymorphic_allocator<char>* allocator);

    template <typename T>
    friend class std::hash;
};

class posix_server_socket_impl : public server_socket_impl {
    socket_address _sa;
    int _protocol;
    pollable_fd _lfd;
    conntrack _conntrack;
    server_socket::load_balancing_algorithm _lba;
    shard_id _fixed_cpu;
    std::pmr::polymorphic_allocator<char>* _allocator;
public:
    explicit posix_server_socket_impl(int protocol, socket_address sa, pollable_fd lfd,
        server_socket::load_balancing_algorithm lba, shard_id fixed_cpu,
        std::pmr::polymorphic_allocator<char>* allocator=memory::malloc_allocator) : _sa(sa), _protocol(protocol), _lfd(std::move(lfd)), _lba(lba), _fixed_cpu(fixed_cpu), _allocator(allocator) {}
    virtual future<accept_result> accept() override;
    virtual void abort_accept() override;
    virtual socket_address local_address() const override;
};

class posix_reuseport_server_socket_impl : public server_socket_impl {
    socket_address _sa;
    int _protocol;
    pollable_fd _lfd;
    std::pmr::polymorphic_allocator<char>* _allocator;
public:
    explicit posix_reuseport_server_socket_impl(int protocol, socket_address sa, pollable_fd lfd,
        std::pmr::polymorphic_allocator<char>* allocator=memory::malloc_allocator) : _sa(sa), _protocol(protocol), _lfd(std::move(lfd)), _allocator(allocator) {}
    virtual future<accept_result> accept() override;
    virtual void abort_accept() override;
    virtual socket_address local_address() const override;
};

class posix_network_stack : public network_stack {
private:
    const bool _reuseport;
protected:
    std::pmr::polymorphic_allocator<char>* _allocator;
public:
    explicit posix_network_stack(const program_options::option_group& opts, std::pmr::polymorphic_allocator<char>* allocator=memory::malloc_allocator);
    virtual server_socket listen(socket_address sa, listen_options opts) override;
    virtual ::seastar::socket socket() override;
    virtual net::udp_channel make_udp_channel(const socket_address&) override;
    virtual net::datagram_channel make_unbound_datagram_channel(sa_family_t) override;
    virtual net::datagram_channel make_bound_datagram_channel(const socket_address& local) override;
    static future<std::unique_ptr<network_stack>> create(const program_options::option_group& opts, std::pmr::polymorphic_allocator<char>* allocator=memory::malloc_allocator) {
        return make_ready_future<std::unique_ptr<network_stack>>(std::unique_ptr<network_stack>(new posix_network_stack(opts, allocator)));
    }
    virtual bool has_per_core_namespace() override { return _reuseport; };
    bool supports_ipv6() const override;
    std::vector<network_interface> network_interfaces() override;
    virtual statistics stats(unsigned scheduling_group_id) override;
    virtual void clear_stats(unsigned scheduling_group_id) override;
};

class posix_ap_network_stack : public posix_network_stack {
private:
    const bool _reuseport;
public:
    posix_ap_network_stack(const program_options::option_group& opts, std::pmr::polymorphic_allocator<char>* allocator=memory::malloc_allocator);
    virtual server_socket listen(socket_address sa, listen_options opts) override;
    static future<std::unique_ptr<network_stack>> create(const program_options::option_group& opts, std::pmr::polymorphic_allocator<char>* allocator=memory::malloc_allocator) {
        return make_ready_future<std::unique_ptr<network_stack>>(std::unique_ptr<network_stack>(new posix_ap_network_stack(opts, allocator)));
    }
};

network_stack_entry register_posix_stack();
}

}
