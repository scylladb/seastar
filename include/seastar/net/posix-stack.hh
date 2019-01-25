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

#include <seastar/core/reactor.hh>
#include <seastar/core/sharded.hh>
#include <seastar/net/stack.hh>
#include <boost/program_options.hpp>

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
            : _host_cpu(engine().cpu_id())
            , _target_cpu(cpu)
            , _lb(make_foreign(std::move(lb))) {}

        handle(const handle&) = delete;
        handle(handle&&) = default;
        ~handle() {
            if (!_lb) {
                return;
            }
            smp::submit_to(_host_cpu, [cpu = _target_cpu, lb = std::move(_lb)] {
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

class posix_data_source_impl final : public data_source_impl {
    lw_shared_ptr<pollable_fd> _fd;
    temporary_buffer<char> _buf;
    size_t _buf_size;
public:
    explicit posix_data_source_impl(lw_shared_ptr<pollable_fd> fd, size_t buf_size = 8192)
        : _fd(std::move(fd)), _buf(buf_size), _buf_size(buf_size) {}
    future<temporary_buffer<char>> get() override;
    future<> close() override;
};

class posix_data_sink_impl : public data_sink_impl {
    lw_shared_ptr<pollable_fd> _fd;
    packet _p;
public:
    explicit posix_data_sink_impl(lw_shared_ptr<pollable_fd> fd) : _fd(std::move(fd)) {}
    using data_sink_impl::put;
    future<> put(packet p) override;
    future<> put(temporary_buffer<char> buf) override;
    future<> close() override;
};

template <transport Transport>
class posix_ap_server_socket_impl : public server_socket_impl {
    struct connection {
        pollable_fd fd;
        socket_address addr;
        connection(pollable_fd xfd, socket_address xaddr) : fd(std::move(xfd)), addr(xaddr) {}
    };
    static thread_local std::unordered_map<::sockaddr_in, promise<connected_socket, socket_address>> sockets;
    static thread_local std::unordered_multimap<::sockaddr_in, connection> conn_q;
    socket_address _sa;
public:
    explicit posix_ap_server_socket_impl(socket_address sa) : _sa(sa) {}
    virtual future<connected_socket, socket_address> accept() override;
    virtual void abort_accept() override;
    static void move_connected_socket(socket_address sa, pollable_fd fd, socket_address addr, conntrack::handle handle);
};
using posix_tcp_ap_server_socket_impl = posix_ap_server_socket_impl<transport::TCP>;
using posix_sctp_ap_server_socket_impl = posix_ap_server_socket_impl<transport::SCTP>;

template <transport Transport>
class posix_server_socket_impl : public server_socket_impl {
    socket_address _sa;
    pollable_fd _lfd;
    conntrack _conntrack;
    server_socket::load_balancing_algorithm _lba;
public:
    explicit posix_server_socket_impl(socket_address sa, pollable_fd lfd, server_socket::load_balancing_algorithm lba) : _sa(sa), _lfd(std::move(lfd)), _lba(lba) {}
    virtual future<connected_socket, socket_address> accept() override;
    virtual void abort_accept() override;
};
using posix_server_tcp_socket_impl = posix_server_socket_impl<transport::TCP>;
using posix_server_sctp_socket_impl = posix_server_socket_impl<transport::SCTP>;

template <transport Transport>
class posix_reuseport_server_socket_impl : public server_socket_impl {
    socket_address _sa;
    pollable_fd _lfd;
public:
    explicit posix_reuseport_server_socket_impl(socket_address sa, pollable_fd lfd) : _sa(sa), _lfd(std::move(lfd)) {}
    virtual future<connected_socket, socket_address> accept() override;
    virtual void abort_accept() override;
};
using posix_reuseport_server_tcp_socket_impl = posix_reuseport_server_socket_impl<transport::TCP>;
using posix_reuseport_server_sctp_socket_impl = posix_reuseport_server_socket_impl<transport::SCTP>;

class posix_network_stack : public network_stack {
private:
    const bool _reuseport;
public:
    explicit posix_network_stack(boost::program_options::variables_map opts) : _reuseport(engine().posix_reuseport_available()) {}
    virtual server_socket listen(socket_address sa, listen_options opts) override;
    virtual ::seastar::socket socket() override;
    virtual net::udp_channel make_udp_channel(ipv4_addr addr) override;
    static future<std::unique_ptr<network_stack>> create(boost::program_options::variables_map opts) {
        return make_ready_future<std::unique_ptr<network_stack>>(std::unique_ptr<network_stack>(new posix_network_stack(opts)));
    }
    virtual bool has_per_core_namespace() override { return _reuseport; };
};

class posix_ap_network_stack : public posix_network_stack {
private:
    const bool _reuseport;
public:
    posix_ap_network_stack(boost::program_options::variables_map opts) : posix_network_stack(std::move(opts)), _reuseport(engine().posix_reuseport_available()) {}
    virtual server_socket listen(socket_address sa, listen_options opts) override;
    static future<std::unique_ptr<network_stack>> create(boost::program_options::variables_map opts) {
        return make_ready_future<std::unique_ptr<network_stack>>(std::unique_ptr<network_stack>(new posix_ap_network_stack(opts)));
    }
};

void register_posix_stack();
}

}
