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

#include "posix-stack.hh"
#include "net.hh"
#include "packet.hh"
#include "api.hh"
#include <netinet/tcp.h>
#include <netinet/sctp.h>

namespace net {

using namespace seastar;

template <transport Transport>
class posix_connected_socket_operations;

template <>
class posix_connected_socket_operations<transport::TCP> {
public:
    void set_nodelay(file_desc& _fd, bool nodelay) {
        _fd.setsockopt(IPPROTO_TCP, TCP_NODELAY, int(nodelay));
    }
    bool get_nodelay(file_desc& _fd) const {
        return _fd.getsockopt<int>(IPPROTO_TCP, TCP_NODELAY);
    }
    void set_keepalive(file_desc& _fd, bool keepalive) {
        _fd.setsockopt(SOL_SOCKET, SO_KEEPALIVE, int(keepalive));
    }
    bool get_keepalive(file_desc& _fd) const {
        return _fd.getsockopt<int>(SOL_SOCKET, SO_KEEPALIVE);
    }
    void set_keepalive_parameters(file_desc& _fd, const keepalive_params& params) {
        const tcp_keepalive_params& pms = boost::get<tcp_keepalive_params>(params);
        _fd.setsockopt(IPPROTO_TCP, TCP_KEEPCNT, pms.count);
        _fd.setsockopt(IPPROTO_TCP, TCP_KEEPIDLE, int(pms.idle.count()));
        _fd.setsockopt(IPPROTO_TCP, TCP_KEEPINTVL, int(pms.interval.count()));
    }
    keepalive_params get_keepalive_parameters(file_desc& _fd) const {
        return tcp_keepalive_params {
            std::chrono::seconds(_fd.getsockopt<int>(IPPROTO_TCP, TCP_KEEPIDLE)),
            std::chrono::seconds(_fd.getsockopt<int>(IPPROTO_TCP, TCP_KEEPINTVL)),
            _fd.getsockopt<unsigned>(IPPROTO_TCP, TCP_KEEPCNT)
        };
    }
};

template <>
class posix_connected_socket_operations<transport::SCTP> {
public:
    void set_nodelay(file_desc& _fd, bool nodelay) {
        _fd.setsockopt(SOL_SCTP, SCTP_NODELAY, int(nodelay));
    }
    bool get_nodelay(file_desc& _fd) const {
        return _fd.getsockopt<int>(SOL_SCTP, SCTP_NODELAY);
    }
    void set_keepalive(file_desc& _fd, bool keepalive) {
        auto heartbeat = _fd.getsockopt<sctp_paddrparams>(SOL_SCTP, SCTP_PEER_ADDR_PARAMS);
        if (keepalive) {
            heartbeat.spp_flags |= SPP_HB_ENABLE;
        } else {
            heartbeat.spp_flags &= ~SPP_HB_ENABLE;
        }
        _fd.setsockopt(SOL_SCTP, SCTP_PEER_ADDR_PARAMS, heartbeat);
    }
    bool get_keepalive(file_desc& _fd) const {
        return _fd.getsockopt<sctp_paddrparams>(SOL_SCTP, SCTP_PEER_ADDR_PARAMS).spp_flags & SPP_HB_ENABLE;
    }
    void set_keepalive_parameters(file_desc& _fd, const keepalive_params& kpms) {
        const sctp_keepalive_params& pms = boost::get<sctp_keepalive_params>(kpms);
        auto params = _fd.getsockopt<sctp_paddrparams>(SOL_SCTP, SCTP_PEER_ADDR_PARAMS);
        params.spp_hbinterval = pms.interval.count() * 1000; // in milliseconds
        params.spp_pathmaxrxt = pms.count;
        _fd.setsockopt(SOL_SCTP, SCTP_PEER_ADDR_PARAMS, params);
    }
    keepalive_params get_keepalive_parameters(file_desc& _fd) const {
        auto params = _fd.getsockopt<sctp_paddrparams>(SOL_SCTP, SCTP_PEER_ADDR_PARAMS);
        return sctp_keepalive_params {
            std::chrono::seconds(params.spp_hbinterval/1000), // in seconds
            params.spp_pathmaxrxt
        };
    }
};

template <transport Transport>
class posix_connected_socket_impl final : public connected_socket_impl, posix_connected_socket_operations<Transport> {
    lw_shared_ptr<pollable_fd> _fd;
    using _ops = posix_connected_socket_operations<Transport>;
private:
    explicit posix_connected_socket_impl(lw_shared_ptr<pollable_fd> fd) : _fd(std::move(fd)) {}
public:
    virtual data_source source() override { return posix_data_source(*_fd); }
    virtual data_sink sink() override { return posix_data_sink(*_fd); }
    virtual future<> shutdown_input() override {
        _fd->shutdown(SHUT_RD);
        return make_ready_future<>();
    }
    virtual future<> shutdown_output() override {
        _fd->shutdown(SHUT_WR);
        return make_ready_future<>();
    }
    virtual void set_nodelay(bool nodelay) override {
        return _ops::set_nodelay(_fd->get_file_desc(), nodelay);
    }
    virtual bool get_nodelay() const override {
        return _ops::get_nodelay(_fd->get_file_desc());
    }
    void set_keepalive(bool keepalive) override {
        return _ops::set_keepalive(_fd->get_file_desc(), keepalive);
    }
    bool get_keepalive() const override {
        return _ops::get_keepalive(_fd->get_file_desc());
    }
    void set_keepalive_parameters(const keepalive_params& p) override {
        return _ops::set_keepalive_parameters(_fd->get_file_desc(), p);
    }
    keepalive_params get_keepalive_parameters() const override {
        return _ops::get_keepalive_parameters(_fd->get_file_desc());
    }
    friend class posix_server_socket_impl<Transport>;
    friend class posix_ap_server_socket_impl<Transport>;
    friend class posix_reuseport_server_socket_impl<Transport>;
    friend class posix_network_stack;
    friend class posix_ap_network_stack;
    friend class posix_socket_impl;
};
using posix_connected_tcp_socket_impl = posix_connected_socket_impl<transport::TCP>;
using posix_connected_sctp_socket_impl = posix_connected_socket_impl<transport::SCTP>;

class posix_socket_impl final : public socket_impl {
    lw_shared_ptr<pollable_fd> _fd;
public:
    posix_socket_impl() = default;

    virtual future<connected_socket> connect(socket_address sa, socket_address local, transport proto = transport::TCP) override {
        _fd = engine().make_pollable_fd(sa, proto);
        return engine().posix_connect(_fd, sa, local).then([fd = _fd, proto]() mutable {
            std::unique_ptr<connected_socket_impl> csi;
            if (proto == transport::TCP) {
                csi.reset(new posix_connected_tcp_socket_impl(std::move(fd)));
            } else {
                csi.reset(new posix_connected_sctp_socket_impl(std::move(fd)));
            }
            return make_ready_future<connected_socket>(connected_socket(std::move(csi)));
        });
    }

    virtual void shutdown() override {
        if (_fd) {
            try {
                _fd->shutdown(SHUT_RDWR);
            } catch (std::system_error& e) {
                if (e.code().value() != ENOTCONN) {
                    throw;
                }
            }
        }
    }
};

template <transport Transport>
future<connected_socket, socket_address>
posix_server_socket_impl<Transport>::accept() {
    return _lfd.accept().then([this] (pollable_fd fd, socket_address sa) {
        static unsigned balance = 0;
        auto cpu = balance++ % smp::count;

        if (cpu == engine().cpu_id()) {
            std::unique_ptr<connected_socket_impl> csi(
                    new posix_connected_socket_impl<Transport>(make_lw_shared(std::move(fd))));
            return make_ready_future<connected_socket, socket_address>(
                    connected_socket(std::move(csi)), sa);
        } else {
            smp::submit_to(cpu, [this, fd = std::move(fd.get_file_desc()), sa] () mutable {
                posix_ap_server_socket_impl<Transport>::move_connected_socket(_sa, pollable_fd(std::move(fd)), sa);
            });
            return accept();
        }
    });
}

template <transport Transport>
void
posix_server_socket_impl<Transport>::abort_accept() {
    _lfd.abort_reader(std::make_exception_ptr(std::system_error(ECONNABORTED, std::system_category())));
}

template <transport Transport>
future<connected_socket, socket_address> posix_ap_server_socket_impl<Transport>::accept() {
    auto conni = conn_q.find(_sa.as_posix_sockaddr_in());
    if (conni != conn_q.end()) {
        connection c = std::move(conni->second);
        conn_q.erase(conni);
        try {
            std::unique_ptr<connected_socket_impl> csi(
                    new posix_connected_socket_impl<Transport>(make_lw_shared(std::move(c.fd))));
            return make_ready_future<connected_socket, socket_address>(connected_socket(std::move(csi)), std::move(c.addr));
        } catch (...) {
            return make_exception_future<connected_socket, socket_address>(std::current_exception());
        }
    } else {
        try {
            auto i = sockets.emplace(std::piecewise_construct, std::make_tuple(_sa.as_posix_sockaddr_in()), std::make_tuple());
            assert(i.second);
            return i.first->second.get_future();
        } catch (...) {
            return make_exception_future<connected_socket, socket_address>(std::current_exception());
        }
    }
}

template <transport Transport>
void
posix_ap_server_socket_impl<Transport>::abort_accept() {
    conn_q.erase(_sa.as_posix_sockaddr_in());
    auto i = sockets.find(_sa.as_posix_sockaddr_in());
    if (i != sockets.end()) {
        i->second.set_exception(std::system_error(ECONNABORTED, std::system_category()));
        sockets.erase(i);
    }
}

template <transport Transport>
future<connected_socket, socket_address>
posix_reuseport_server_socket_impl<Transport>::accept() {
    return _lfd.accept().then([this] (pollable_fd fd, socket_address sa) {
        std::unique_ptr<connected_socket_impl> csi(
                new posix_connected_socket_impl<Transport>(make_lw_shared(std::move(fd))));
        return make_ready_future<connected_socket, socket_address>(
            connected_socket(std::move(csi)), sa);
    });
}

template <transport Transport>
void
posix_reuseport_server_socket_impl<Transport>::abort_accept() {
    _lfd.abort_reader(std::make_exception_ptr(std::system_error(ECONNABORTED, std::system_category())));
}

template <transport Transport>
void  posix_ap_server_socket_impl<Transport>::move_connected_socket(socket_address sa, pollable_fd fd, socket_address addr) {
    auto i = sockets.find(sa.as_posix_sockaddr_in());
    if (i != sockets.end()) {
        try {
            std::unique_ptr<connected_socket_impl> csi(new posix_connected_socket_impl<Transport>(make_lw_shared(std::move(fd))));
            i->second.set_value(connected_socket(std::move(csi)), std::move(addr));
        } catch (...) {
            i->second.set_exception(std::current_exception());
        }
        sockets.erase(i);
    } else {
        conn_q.emplace(std::piecewise_construct, std::make_tuple(sa.as_posix_sockaddr_in()), std::make_tuple(std::move(fd), std::move(addr)));
    }
}

data_source posix_data_source(pollable_fd& fd) {
    return data_source(std::make_unique<posix_data_source_impl>(fd));
}

future<temporary_buffer<char>>
posix_data_source_impl::get() {
    return _fd.read_some(_buf.get_write(), _buf_size).then([this] (size_t size) {
        _buf.trim(size);
        auto ret = std::move(_buf);
        _buf = temporary_buffer<char>(_buf_size);
        return make_ready_future<temporary_buffer<char>>(std::move(ret));
    });
}

data_sink posix_data_sink(pollable_fd& fd) {
    return data_sink(std::make_unique<posix_data_sink_impl>(fd));
}

std::vector<struct iovec> to_iovec(const packet& p) {
    std::vector<struct iovec> v;
    v.reserve(p.nr_frags());
    for (auto&& f : p.fragments()) {
        v.push_back({.iov_base = f.base, .iov_len = f.size});
    }
    return v;
}

std::vector<iovec> to_iovec(std::vector<temporary_buffer<char>>& buf_vec) {
    std::vector<iovec> v;
    v.reserve(buf_vec.size());
    for (auto& buf : buf_vec) {
        v.push_back({.iov_base = buf.get_write(), .iov_len = buf.size()});
    }
    return v;
}

future<>
posix_data_sink_impl::put(temporary_buffer<char> buf) {
    return _fd.write_all(buf.get(), buf.size()).then([d = buf.release()] {});
}

future<>
posix_data_sink_impl::put(packet p) {
    _p = std::move(p);
    return _fd.write_all(_p).then([this] { _p.reset(); });
}

server_socket
posix_network_stack::listen(socket_address sa, listen_options opt) {
    if (opt.proto == transport::TCP) {
        return _reuseport ?
            server_socket(std::make_unique<posix_reuseport_server_tcp_socket_impl>(sa, engine().posix_listen(sa, opt)))
            :
            server_socket(std::make_unique<posix_server_tcp_socket_impl>(sa, engine().posix_listen(sa, opt)));
    } else {
        return _reuseport ?
            server_socket(std::make_unique<posix_reuseport_server_sctp_socket_impl>(sa, engine().posix_listen(sa, opt)))
            :
            server_socket(std::make_unique<posix_server_sctp_socket_impl>(sa, engine().posix_listen(sa, opt)));
    }
}

::seastar::socket posix_network_stack::socket() {
    return ::seastar::socket(std::make_unique<posix_socket_impl>());
}

template<transport Transport>
thread_local std::unordered_map<::sockaddr_in, promise<connected_socket, socket_address>> posix_ap_server_socket_impl<Transport>::sockets;
template<transport Transport>
thread_local std::unordered_multimap<::sockaddr_in, typename posix_ap_server_socket_impl<Transport>::connection> posix_ap_server_socket_impl<Transport>::conn_q;

server_socket
posix_ap_network_stack::listen(socket_address sa, listen_options opt) {
    if (opt.proto == transport::TCP) {
        return _reuseport ?
            server_socket(std::make_unique<posix_reuseport_server_tcp_socket_impl>(sa, engine().posix_listen(sa, opt)))
            :
            server_socket(std::make_unique<posix_tcp_ap_server_socket_impl>(sa));
    } else {
        return _reuseport ?
            server_socket(std::make_unique<posix_reuseport_server_sctp_socket_impl>(sa, engine().posix_listen(sa, opt)))
            :
            server_socket(std::make_unique<posix_sctp_ap_server_socket_impl>(sa));
    }
}

struct cmsg_with_pktinfo {
    struct cmsghdrcmh;
    struct in_pktinfo pktinfo;
};

class posix_udp_channel : public udp_channel_impl {
private:
    static constexpr int MAX_DATAGRAM_SIZE = 65507;
    struct recv_ctx {
        struct msghdr _hdr;
        struct iovec _iov;
        socket_address _src_addr;
        char* _buffer;
        cmsg_with_pktinfo _cmsg;

        recv_ctx() {
            memset(&_hdr, 0, sizeof(_hdr));
            _hdr.msg_iov = &_iov;
            _hdr.msg_iovlen = 1;
            _hdr.msg_name = &_src_addr.u.sa;
            _hdr.msg_namelen = sizeof(_src_addr.u.sas);
            memset(&_cmsg, 0, sizeof(_cmsg));
            _hdr.msg_control = &_cmsg;
            _hdr.msg_controllen = sizeof(_cmsg);
        }

        void prepare() {
            _buffer = new char[MAX_DATAGRAM_SIZE];
            _iov.iov_base = _buffer;
            _iov.iov_len = MAX_DATAGRAM_SIZE;
        }
    };
    struct send_ctx {
        struct msghdr _hdr;
        std::vector<struct iovec> _iovecs;
        socket_address _dst;
        packet _p;

        send_ctx() {
            memset(&_hdr, 0, sizeof(_hdr));
            _hdr.msg_name = &_dst.u.sa;
            _hdr.msg_namelen = sizeof(_dst.u.sas);
        }

        void prepare(ipv4_addr dst, packet p) {
            _dst = make_ipv4_address(dst);
            _p = std::move(p);
            _iovecs = std::move(to_iovec(_p));
            _hdr.msg_iov = _iovecs.data();
            _hdr.msg_iovlen = _iovecs.size();
        }
    };
    std::unique_ptr<pollable_fd> _fd;
    ipv4_addr _address;
    recv_ctx _recv;
    send_ctx _send;
    bool _closed;
public:
    posix_udp_channel(ipv4_addr bind_address)
            : _closed(false) {
        auto sa = make_ipv4_address(bind_address);
        file_desc fd = file_desc::socket(sa.u.sa.sa_family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        fd.setsockopt(SOL_IP, IP_PKTINFO, true);
        if (engine().posix_reuseport_available()) {
            fd.setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
        }
        fd.bind(sa.u.sa, sizeof(sa.u.sas));
        _address = ipv4_addr(fd.get_address());
        _fd = std::make_unique<pollable_fd>(std::move(fd));
    }
    virtual ~posix_udp_channel() { if (!_closed) close(); };
    virtual future<udp_datagram> receive() override;
    virtual future<> send(ipv4_addr dst, const char *msg);
    virtual future<> send(ipv4_addr dst, packet p);
    virtual void close() override {
        _closed = true;
        _fd->abort_reader(std::make_exception_ptr(std::system_error(EPIPE, std::system_category())));
        _fd->abort_writer(std::make_exception_ptr(std::system_error(EPIPE, std::system_category())));
        _fd.reset();
    }
    virtual bool is_closed() const override { return _closed; }
};

future<> posix_udp_channel::send(ipv4_addr dst, const char *message) {
    auto len = strlen(message);
    return _fd->sendto(make_ipv4_address(dst), message, len)
            .then([len] (size_t size) { assert(size == len); });
}

future<> posix_udp_channel::send(ipv4_addr dst, packet p) {
    auto len = p.len();
    _send.prepare(dst, std::move(p));
    return _fd->sendmsg(&_send._hdr)
            .then([len] (size_t size) { assert(size == len); });
}

udp_channel
posix_network_stack::make_udp_channel(ipv4_addr addr) {
    return udp_channel(std::make_unique<posix_udp_channel>(addr));
}

class posix_datagram : public udp_datagram_impl {
private:
    ipv4_addr _src;
    ipv4_addr _dst;
    packet _p;
public:
    posix_datagram(ipv4_addr src, ipv4_addr dst, packet p) : _src(src), _dst(dst), _p(std::move(p)) {}
    virtual ipv4_addr get_src() override { return _src; }
    virtual ipv4_addr get_dst() override { return _dst; }
    virtual uint16_t get_dst_port() override { return _dst.port; }
    virtual packet& get_data() override { return _p; }
};

future<udp_datagram>
posix_udp_channel::receive() {
    _recv.prepare();
    return _fd->recvmsg(&_recv._hdr).then([this] (size_t size) {
        auto dst = ipv4_addr(_recv._cmsg.pktinfo.ipi_addr.s_addr, _address.port);
        return make_ready_future<udp_datagram>(udp_datagram(std::make_unique<posix_datagram>(
            _recv._src_addr, dst, packet(fragment{_recv._buffer, size}, make_deleter([buf = _recv._buffer] { delete[] buf; })))));
    });
}

}
