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
 *
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <boost/asio/ip/address_v4.hpp>
#include <arpa/inet.h>
#include <unordered_map>
#include <cstdint>
#include <array>
#include <map>
#include <list>
#include <chrono>
#endif

#include <seastar/util/internal/array_map.hh>
#include <seastar/net/byteorder.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/net/arp.hh>
#include <seastar/net/ip_checksum.hh>
#include <seastar/net/const.hh>
#include <seastar/net/packet-util.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/net/toeplitz.hh>
#include <seastar/net/udp.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/util/modules.hh>

#include "ipv4_address.hh"
#include "ipv6_address.hh"

namespace seastar {

namespace net {

class ipv4;
template <ip_protocol_num ProtoNum>
class ipv4_l4;

template <typename InetTraits>
class tcp;

struct ipv4_traits {
    using address_type = ipv4_address;
    using inet_type = ipv4_l4<ip_protocol_num::tcp>;
    struct l4packet {
        ipv4_address to;
        packet p;
        ethernet_address e_dst;
        ip_protocol_num proto_num;
    };
    using packet_provider_type = std::function<std::optional<l4packet> ()>;
    static void tcp_pseudo_header_checksum(checksummer& csum, ipv4_address src, ipv4_address dst, uint16_t len) {
        csum.sum_many(src.ip.raw, dst.ip.raw, uint8_t(0), uint8_t(ip_protocol_num::tcp), len);
    }
    static void udp_pseudo_header_checksum(checksummer& csum, ipv4_address src, ipv4_address dst, uint16_t len) {
        csum.sum_many(src.ip.raw, dst.ip.raw, uint8_t(0), uint8_t(ip_protocol_num::udp), len);
    }
    static constexpr uint8_t ip_hdr_len_min = ipv4_hdr_len_min;
};

template <ip_protocol_num ProtoNum>
class ipv4_l4 {
public:
    ipv4& _inet;
public:
    ipv4_l4(ipv4& inet) : _inet(inet) {}
    void register_packet_provider(ipv4_traits::packet_provider_type func);
    future<ethernet_address> get_l2_dst_address(ipv4_address to);
    const ipv4& inet() const {
        return _inet;
    }
};

class ip_protocol {
public:
    virtual ~ip_protocol() {}
    virtual void received(packet p, ipv4_address from, ipv4_address to) = 0;
    virtual bool forward(forward_hash& out_hash_data, packet& p, size_t off) {
      std::ignore = out_hash_data;
      std::ignore = p;
      std::ignore = off;
      return true;
    }
};

template <typename InetTraits>
struct l4connid {
    using ipaddr = typename InetTraits::address_type;
    using inet_type = typename InetTraits::inet_type;
    struct connid_hash;

    ipaddr local_ip;
    ipaddr foreign_ip;
    uint16_t local_port;
    uint16_t foreign_port;

    bool operator==(const l4connid& x) const {
        return local_ip == x.local_ip
                && foreign_ip == x.foreign_ip
                && local_port == x.local_port
                && foreign_port == x.foreign_port;
    }

    uint32_t hash(rss_key_type rss_key) {
        forward_hash hash_data;
        hash_data.push_back(hton(foreign_ip.ip));
        hash_data.push_back(hton(local_ip.ip));
        hash_data.push_back(hton(foreign_port));
        hash_data.push_back(hton(local_port));
        return toeplitz_hash(rss_key, hash_data);
    }
};

class ipv4_tcp final : public ip_protocol {
    ipv4_l4<ip_protocol_num::tcp> _inet_l4;
    std::unique_ptr<tcp<ipv4_traits>> _tcp;
public:
    ipv4_tcp(ipv4& inet);
    ~ipv4_tcp();
    virtual void received(packet p, ipv4_address from, ipv4_address to) override;
    virtual bool forward(forward_hash& out_hash_data, packet& p, size_t off) override;
    friend class ipv4;
};

struct icmp_hdr {
    enum class msg_type : uint8_t {
        echo_reply = 0,
        echo_request = 8,
    };
    msg_type type;
    uint8_t code;
    packed<uint16_t> csum;
    packed<uint32_t> rest;
    template <typename Adjuster>
    auto adjust_endianness(Adjuster a) {
        return a(csum);
    }
} __attribute__((packed));


class icmp {
public:
    using ipaddr = ipv4_address;
    using inet_type = ipv4_l4<ip_protocol_num::icmp>;
    explicit icmp(inet_type& inet) : _inet(inet) {
        _inet.register_packet_provider([this] {
            std::optional<ipv4_traits::l4packet> l4p;
            if (!_packetq.empty()) {
                l4p = std::move(_packetq.front());
                _packetq.pop_front();
                _queue_space.signal(l4p.value().p.len());
            }
            return l4p;
        });
    }
    void received(packet p, ipaddr from, ipaddr to);
private:
    inet_type& _inet;
    circular_buffer<ipv4_traits::l4packet> _packetq;
    semaphore _queue_space = {212992};
};

class ipv4_icmp final : public ip_protocol {
    ipv4_l4<ip_protocol_num::icmp> _inet_l4;
    icmp _icmp;
public:
    ipv4_icmp(ipv4& inet) : _inet_l4(inet), _icmp(_inet_l4) {}
    virtual void received(packet p, ipv4_address from, ipv4_address to) {
        _icmp.received(std::move(p), from, to);
    }
    friend class ipv4;
};

class ipv4_udp : public ip_protocol {
    using connid = l4connid<ipv4_traits>;
    using connid_hash = typename connid::connid_hash;

public:
    static const int default_queue_size;
private:
    static const uint16_t min_anonymous_port = 32768;
    ipv4 &_inet;
    std::unordered_map<uint16_t, lw_shared_ptr<udp_channel_state>> _channels;
    int _queue_size = default_queue_size;
    uint16_t _next_anonymous_port = min_anonymous_port;
    circular_buffer<ipv4_traits::l4packet> _packetq;
private:
    uint16_t next_port(uint16_t port);
public:
    class registration {
    private:
        ipv4_udp &_proto;
        uint16_t _port;
    public:
        registration(ipv4_udp &proto, uint16_t port) : _proto(proto), _port(port) {};

        void unregister() {
            _proto._channels.erase(_proto._channels.find(_port));
        }

        uint16_t port() const {
            return _port;
        }
    };

    ipv4_udp(ipv4& inet);
    udp_channel make_channel(ipv4_addr addr);
    virtual void received(packet p, ipv4_address from, ipv4_address to) override;
    void send(uint16_t src_port, ipv4_addr dst, packet &&p);
    bool forward(forward_hash& out_hash_data, packet& p, size_t off) override;
    void set_queue_size(int size) { _queue_size = size; }

    const ipv4& inet() const {
        return _inet;
    }
};

struct ip_hdr;

struct ip_packet_filter {
    virtual ~ip_packet_filter() {};
    virtual future<> handle(packet& p, ip_hdr* iph, ethernet_address from, bool & handled) = 0;
};

struct ipv4_frag_id {
    struct hash;
    ipv4_address src_ip;
    ipv4_address dst_ip;
    uint16_t identification;
    uint8_t protocol;
    bool operator==(const ipv4_frag_id& x) const {
        return src_ip == x.src_ip &&
               dst_ip == x.dst_ip &&
               identification == x.identification &&
               protocol == x.protocol;
    }
};

struct ipv4_frag_id::hash : private std::hash<ipv4_address>,
    private std::hash<uint16_t>, private std::hash<uint8_t> {
    size_t operator()(const ipv4_frag_id& id) const noexcept {
        using h1 = std::hash<ipv4_address>;
        using h2 = std::hash<uint16_t>;
        using h3 = std::hash<uint8_t>;
        return h1::operator()(id.src_ip) ^
               h1::operator()(id.dst_ip) ^
               h2::operator()(id.identification) ^
               h3::operator()(id.protocol);
    }
};

struct ipv4_tag {};
using ipv4_packet_merger = packet_merger<uint32_t, ipv4_tag>;

class ipv4 {
public:
    using clock_type = lowres_clock;
    using address_type = ipv4_address;
    using proto_type = uint16_t;
    static address_type broadcast_address() { return ipv4_address(0xffffffff); }
    static proto_type arp_protocol_type() { return proto_type(eth_protocol_num::ipv4); }
private:
    interface* _netif;
    std::vector<ipv4_traits::packet_provider_type> _pkt_providers;
    arp _global_arp;
    arp_for<ipv4> _arp;
    ipv4_address _host_address;
    ipv4_address _gw_address;
    ipv4_address _netmask;
    l3_protocol _l3;
    ipv4_tcp _tcp;
    ipv4_icmp _icmp;
    ipv4_udp _udp;
    internal::array_map<ip_protocol*, 256> _l4;
    ip_packet_filter * _packet_filter = nullptr;
    struct frag {
        packet header;
        ipv4_packet_merger data;
        clock_type::time_point rx_time;
        uint32_t mem_size = 0;
        // fragment with MF == 0 inidates it is the last fragment
        bool last_frag_received = false;

        packet get_assembled_packet(ethernet_address from, ethernet_address to);
        int32_t merge(ip_hdr &h, uint16_t offset, packet p);
        bool is_complete();
    };
    std::unordered_map<ipv4_frag_id, frag, ipv4_frag_id::hash> _frags;
    std::list<ipv4_frag_id> _frags_age;
    static constexpr std::chrono::seconds _frag_timeout{30};
    static constexpr uint32_t _frag_low_thresh{3 * 1024 * 1024};
    static constexpr uint32_t _frag_high_thresh{4 * 1024 * 1024};
    uint32_t _frag_mem{0};
    timer<lowres_clock> _frag_timer;
    circular_buffer<l3_protocol::l3packet> _packetq;
    unsigned _pkt_provider_idx = 0;
    metrics::metric_groups _metrics;
private:
    future<> handle_received_packet(packet p, ethernet_address from);
    bool forward(forward_hash& out_hash_data, packet& p, size_t off);
    std::optional<l3_protocol::l3packet> get_packet();
    bool in_my_netmask(ipv4_address a) const;
    void frag_limit_mem();
    void frag_timeout();
    void frag_drop(ipv4_frag_id frag_id, uint32_t dropped_size);
    void frag_arm(clock_type::time_point now) {
        auto tp = now + _frag_timeout;
        _frag_timer.arm(tp);
    }
    void frag_arm() {
        auto now = clock_type::now();
        frag_arm(now);
    }
public:
    explicit ipv4(interface* netif);
    void set_host_address(ipv4_address ip);
    ipv4_address host_address() const;
    void set_gw_address(ipv4_address ip);
    ipv4_address gw_address() const;
    void set_netmask_address(ipv4_address ip);
    ipv4_address netmask_address() const;
    interface * netif() const {
        return _netif;
    }
    // TODO or something. Should perhaps truly be a list
    // of filters. With ordering. And blackjack. Etc.
    // But for now, a simple single raw pointer suffices
    void set_packet_filter(ip_packet_filter *);
    ip_packet_filter * packet_filter() const;
    void send(ipv4_address to, ip_protocol_num proto_num, packet p, ethernet_address e_dst);
    tcp<ipv4_traits>& get_tcp() { return *_tcp._tcp; }
    ipv4_udp& get_udp() { return _udp; }
    void register_l4(proto_type id, ip_protocol* handler);
    const net::hw_features& hw_features() const { return _netif->hw_features(); }
    static bool needs_frag(packet& p, ip_protocol_num proto_num, net::hw_features hw_features);
    void learn(ethernet_address l2, ipv4_address l3) {
        _arp.learn(l2, l3);
    }
    void register_packet_provider(ipv4_traits::packet_provider_type&& func) {
        _pkt_providers.push_back(std::move(func));
    }
    future<ethernet_address> get_l2_dst_address(ipv4_address to);
};

template <ip_protocol_num ProtoNum>
inline
void ipv4_l4<ProtoNum>::register_packet_provider(ipv4_traits::packet_provider_type func) {
    _inet.register_packet_provider([func = std::move(func)] {
        auto l4p = func();
        if (l4p) {
            l4p.value().proto_num = ProtoNum;
        }
        return l4p;
    });
}

template <ip_protocol_num ProtoNum>
inline
future<ethernet_address> ipv4_l4<ProtoNum>::get_l2_dst_address(ipv4_address to) {
    return _inet.get_l2_dst_address(to);
}

struct ip_hdr {
    uint8_t ihl : 4;
    uint8_t ver : 4;
    uint8_t dscp : 6;
    uint8_t ecn : 2;
    packed<uint16_t> len;
    packed<uint16_t> id;
    packed<uint16_t> frag;
    enum class frag_bits : uint8_t { mf = 13, df = 14, reserved = 15, offset_shift = 3 };
    uint8_t ttl;
    uint8_t ip_proto;
    packed<uint16_t> csum;
    ipv4_address src_ip;
    ipv4_address dst_ip;
    uint8_t options[0];
    template <typename Adjuster>
    auto adjust_endianness(Adjuster a) {
        return a(len, id, frag, csum, src_ip, dst_ip);
    }
    bool mf() { return frag & (1 << uint8_t(frag_bits::mf)); }
    bool df() { return frag & (1 << uint8_t(frag_bits::df)); }
    uint16_t offset() { return frag << uint8_t(frag_bits::offset_shift); }
} __attribute__((packed));

template <typename InetTraits>
struct l4connid<InetTraits>::connid_hash : private std::hash<ipaddr>, private std::hash<uint16_t> {
    size_t operator()(const l4connid<InetTraits>& id) const noexcept {
        using h1 = std::hash<ipaddr>;
        using h2 = std::hash<uint16_t>;
        return h1::operator()(id.local_ip)
            ^ h1::operator()(id.foreign_ip)
            ^ h2::operator()(id.local_port)
            ^ h2::operator()(id.foreign_port);
    }
};

void arp_learn(ethernet_address l2, ipv4_address l3);

}

}
