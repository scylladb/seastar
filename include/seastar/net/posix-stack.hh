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
#include <chrono>
#include <limits>
#include <tuple>
#include <unordered_set>
#include <vector>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/internal/pollable_fd.hh>
#include <seastar/net/stack.hh>
#include <seastar/core/polymorphic_temporary_buffer.hh>
#include <seastar/core/internal/buffer_allocator.hh>
#include <seastar/util/program-options.hh>

#include <unordered_set>

namespace seastar {

namespace internal {

// Holder of iovec-s for vectorized IO syscalls
// Gets populated from temporary_buffers, providing deleter object
// that controls the lifetime of the buffers it points to
struct wrapped_iovecs {
    std::vector<iovec> v;
    std::pair<size_t, deleter> populate(std::span<temporary_buffer<char>> bufs);

    wrapped_iovecs() = default;
    wrapped_iovecs(const wrapped_iovecs&) = delete;
    wrapped_iovecs(wrapped_iovecs&&) = delete;
};

}

namespace net {

using namespace seastar;

// We can't keep this in any of the socket servers as instance members, because a connection can
// outlive the socket server. To avoid having the whole socket_server tracked as a shared pointer,
// we will have a conntrack structure.
//
// Right now this class is used by the posix_server_socket_impl, but it could be used by any other.
class conntrack {
public:
    // Identifies a listening endpoint whose accepted connections are tracked:
    // the protocol and the address the application asked to listen on.
    using endpoint = std::tuple<int, socket_address>;
private:
    class load_balancer {
        std::vector<unsigned> _cpu_load;
    public:
        load_balancer() : _cpu_load(size_t(this_smp_shard_count()), 0) {}
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

    // Exact counts of the connections this shard currently hosts, per
    // endpoint. Only the owning shard touches its counters, so no
    // synchronization is involved; other shards obtain a snapshot with
    // collect_hosted(), paying one cross-shard message per shard. Entries are
    // never erased (outstanding handles point into the map), so the map's
    // size is bounded by the number of distinct endpoints listened on over
    // the process lifetime.
    static long& hosted_counter(const endpoint& ep);
public:
    // The hosted-connection count of the calling shard for the given endpoint.
    static long hosted_count(const endpoint& ep);
    // The count that collect_hosted() reports for a shard without a listener
    // for the endpoint. Such a shard drops the connections handed to it, so a
    // balancer must never choose it. It is half the maximum, so that adding
    // placements to it cannot overflow.
    static constexpr long not_listening = std::numeric_limits<long>::max() / 2;
    // A snapshot of every shard's hosted-connection count for the given
    // endpoint, indexed by shard id, with not_listening for a shard without a
    // listener for it. The counts are collected in parallel, but
    // are not a consistent cross-shard view: each shard's count is read at
    // whatever time its message is served.
    static future<std::vector<long>> collect_hosted(endpoint ep);

    class handle {
        // Takes the connection back out of its hosting shard's counter.
        struct hosted_release {
            shard_id cpu;
            void operator()(long* counter) const noexcept {
                if (cpu == this_shard_id()) {
                    --*counter;
                } else {
                    // FIXME: future is discarded
                    (void)smp::submit_to(cpu, [counter] { --*counter; });
                }
            }
        };
        shard_id _host_cpu;
        shard_id _target_cpu;
        foreign_ptr<lw_shared_ptr<load_balancer>> _lb;
        std::unique_ptr<long, hosted_release> _hosted;
    public:
        handle() : _lb(nullptr) {}
        handle(shard_id cpu, lw_shared_ptr<load_balancer> lb)
            : _host_cpu(this_shard_id())
            , _target_cpu(cpu)
            , _lb(make_foreign(std::move(lb))) {}
        // Counts a connection that arrived on its hosting shard in that
        // shard's hosted counter for ep; the handle takes it back out.
        explicit handle(const endpoint& ep)
            : _lb(nullptr)
            , _hosted(&++hosted_counter(ep), hosted_release{this_shard_id()}) {}

        handle(const handle&) = delete;
        handle(handle&&) = default;
        handle& operator=(const handle&) = delete;
        handle& operator=(handle&&) = default;
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

// A reuseport listener's private balancer: an estimate of how the connections
// of one listening endpoint are spread across shards, used to decide whether a
// connection accepted on this shard stays here or is handed off.
//
// With SO_REUSEPORT every shard accepts independently, so no single balancer
// sees all connections. Each listener tracks its own placements in its view
// and occasionally corrects the view with an exact snapshot of the real
// per-shard counts (conntrack::collect_hosted()). The snapshot probability is
// min(refresh_weight / average-connections-per-shard, 1): the first few
// connections per shard are placed with accurate information, while at higher
// connection counts the collection cost is amortized over many accepts.
class sampled_conntrack : public enable_lw_shared_from_this<sampled_conntrack> {
public:
    static constexpr long refresh_weight = 10;
    // Upper bound on the view's age; see maybe_collect().
    static constexpr std::chrono::milliseconds max_view_age{100};
private:
    conntrack::endpoint _endpoint;
    std::vector<long> _view;    // estimated hosted connections, indexed by shard
    std::vector<long> _placed;  // our placements since the in-flight collection started
    bool _collecting = false;
    lowres_clock::time_point _refreshed = lowres_clock::time_point::min();
public:
    explicit sampled_conntrack(conntrack::endpoint ep);
    // Chooses the shard to host a newly accepted connection, accounts for the
    // placement in the local view, and possibly triggers an asynchronous
    // refresh of the view.
    shard_id place();
private:
    shard_id least_loaded() const;
    void maybe_collect();
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
#if SEASTAR_API_LEVEL >= 9
    internal::wrapped_iovecs _vecs;
#else
    packet _p{net::packet::make_null_packet()};
#endif
public:
    explicit posix_data_sink_impl(pollable_fd fd) : _fd(std::move(fd)) {}
#if SEASTAR_API_LEVEL >= 9
    future<> put(std::span<temporary_buffer<char>>) override;
#else
    using data_sink_impl::put;
    future<> put(packet p) override;
    future<> put(temporary_buffer<char> buf) override;
#endif
    future<> close() override;
    bool can_batch_flushes() const noexcept override { return true; }
    void on_batch_flush_error() noexcept override;
};

struct proxy_data {
    socket_address remote_address;
    socket_address local_address;
};

class posix_ap_server_socket_impl : public server_socket_impl {
    using protocol_and_socket_address = conntrack::endpoint;
    struct connection {
        pollable_fd fd;
        socket_address addr;
        conntrack::handle connection_tracking_handle;
        std::optional<proxy_data> proxy_protocol_header_opt;
        connection(pollable_fd xfd, socket_address xaddr, conntrack::handle cth, std::optional<proxy_data> addr_data_opt) : fd(std::move(xfd)), addr(xaddr), connection_tracking_handle(std::move(cth)), proxy_protocol_header_opt(std::move(addr_data_opt)) {}
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
    static void move_connected_socket(int protocol, socket_address sa, pollable_fd fd, socket_address addr, conntrack::handle handle, std::optional<proxy_data> addr_data_opt, std::pmr::polymorphic_allocator<char>* allocator);
    // Whether a listener on this shard claims the endpoint.
    static bool listening(const protocol_and_socket_address& ep);

    // The delivery maps above are shared with posix_reuseport_server_socket_impl,
    // whose listeners receive connections handed off by other shards through
    // move_connected_socket() too. The helpers below operate on those maps.

    // Returns a queued connection for the endpoint if one is pending, otherwise
    // registers the caller as the endpoint's waiter (at most one may exist) and
    // resolves once a connection is delivered.
    static future<accept_result> wait_connection(int protocol, const socket_address& sa, std::pmr::polymorphic_allocator<char>* allocator);
    // Whether a waiter is currently registered for the endpoint.
    static bool has_waiter(int protocol, const socket_address& sa);
    // Fails the endpoint's waiter with the given exception. Returns false when
    // no waiter is registered.
    static bool fail_waiter(int protocol, const socket_address& sa, std::exception_ptr ex);
    // Drops the endpoint's queued connections and fails its waiter, if any,
    // with ECONNABORTED.
    static void abort_pending(int protocol, const socket_address& sa);
    // Claims the endpoint for a listener; throws EADDRINUSE when the endpoint
    // is already claimed on this shard.
    static void register_endpoint(int protocol, const socket_address& sa);
    static void unregister_endpoint(int protocol, const socket_address& sa);

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
    bool _proxy_protocol;
    std::pmr::polymorphic_allocator<char>* _allocator;
public:
    explicit posix_server_socket_impl(int protocol, socket_address sa, pollable_fd lfd,
        server_socket::load_balancing_algorithm lba, shard_id fixed_cpu,
        bool proxy_protocol,
        std::pmr::polymorphic_allocator<char>* allocator=memory::malloc_allocator) : _sa(sa), _protocol(protocol), _lfd(std::move(lfd)), _lba(lba), _fixed_cpu(fixed_cpu), _proxy_protocol(proxy_protocol), _allocator(allocator) {}
    virtual future<accept_result> accept() override;
    virtual void abort_accept() override;
    virtual socket_address local_address() const override;
};

class posix_reuseport_server_socket_impl : public server_socket_impl {
    // Shared with the detached kernel-accept continuation, which can be in
    // flight while the listener is destroyed (see arm()).
    struct state {
        int protocol;
        socket_address sa;
        pollable_fd lfd;
        std::pmr::polymorphic_allocator<char>* allocator;
        lw_shared_ptr<sampled_conntrack> tracker;
        // A failure of the kernel accept, or of routing its connection,
        // observed while no accept() was waiting, reported to the next
        // accept() call. One slot suffices: after a failure the kernel accept
        // is not re-armed until an accept() call first consumes the error.
        std::exception_ptr pending_error;
        bool accept_armed = false;
        bool aborted = false;
    };
    lw_shared_ptr<state> _state;
public:
    explicit posix_reuseport_server_socket_impl(int protocol, socket_address sa, pollable_fd lfd,
        std::pmr::polymorphic_allocator<char>* allocator=memory::malloc_allocator);
    ~posix_reuseport_server_socket_impl();
    virtual future<accept_result> accept() override;
    virtual void abort_accept() override;
    virtual socket_address local_address() const override;
private:
    // Starts one kernel accept on the listener, unless one is already running.
    // Its completion routes the connection to the shard chosen by the tracker
    // and re-arms while a local accept() call is left waiting.
    static void arm(lw_shared_ptr<state> st);
};

class posix_network_stack : public network_stack {
protected:
    const bool _reuseport;
    const bool _sock_need_nonblock;
    std::pmr::polymorphic_allocator<char>* _allocator;
public:
    explicit posix_network_stack(const program_options::option_group& opts, std::pmr::polymorphic_allocator<char>* allocator=memory::malloc_allocator);
    virtual server_socket listen(socket_address sa, listen_options opts) override;
    virtual ::seastar::socket socket() override;
    virtual net::datagram_channel make_unbound_datagram_channel(sa_family_t) override;
    virtual net::datagram_channel make_bound_datagram_channel(const socket_address& local) override;
    static future<std::unique_ptr<network_stack>> create(const program_options::option_group& opts, std::pmr::polymorphic_allocator<char>* allocator=memory::malloc_allocator) {
        return make_ready_future<std::unique_ptr<network_stack>>(std::unique_ptr<network_stack>(new posix_network_stack(opts, allocator)));
    }
    virtual bool has_per_core_namespace() override { return false; };
    bool supports_ipv6() const override;
    std::vector<network_interface> network_interfaces() override;
    virtual statistics stats(unsigned scheduling_group_id) override;
    virtual void clear_stats(unsigned scheduling_group_id) override;
};

class posix_ap_network_stack : public posix_network_stack {
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
