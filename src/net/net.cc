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

#ifdef SEASTAR_MODULE
module;
#endif

#include <boost/asio/ip/address_v4.hpp>
#include <boost/algorithm/string.hpp>
#include <map>
#include <utility>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/net/net.hh>
#include <seastar/net/toeplitz.hh>
#include <seastar/core/internal/poll.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/print.hh>
#include <seastar/net/inet_address.hh>
#endif
#include <seastar/util/assert.hh>

namespace seastar {

static_assert(std::is_nothrow_default_constructible_v<ipv4_addr>);
static_assert(std::is_nothrow_copy_constructible_v<ipv4_addr>);
static_assert(std::is_nothrow_move_constructible_v<ipv4_addr>);

static_assert(std::is_nothrow_default_constructible_v<ipv6_addr>);
static_assert(std::is_nothrow_copy_constructible_v<ipv6_addr>);
static_assert(std::is_nothrow_move_constructible_v<ipv6_addr>);

using std::move;

ipv4_addr::ipv4_addr(const std::string &addr) {
    std::vector<std::string> items;
    boost::split(items, addr, boost::is_any_of(":"));
    if (items.size() == 1) {
        ip = boost::asio::ip::make_address_v4(addr).to_uint();
        port = 0;
    } else if (items.size() == 2) {
        ip = boost::asio::ip::make_address_v4(items[0]).to_uint();
        port = std::stoul(items[1]);
    } else {
        throw std::invalid_argument("invalid format: " + addr);
    }
}

ipv4_addr::ipv4_addr(const std::string &addr, uint16_t port_) : ip(boost::asio::ip::make_address_v4(addr).to_uint()), port(port_) {}

ipv4_addr::ipv4_addr(const net::inet_address& a, uint16_t port)
    : ipv4_addr(::in_addr(a), port)
{}

ipv4_addr::ipv4_addr(const socket_address &sa) noexcept
    : ipv4_addr(sa.addr(), sa.port())
{}

ipv4_addr::ipv4_addr(const ::in_addr& in, uint16_t p) noexcept
    : ip(net::ntoh(in.s_addr)), port(p)
{}

namespace net {

inline
bool qp::poll_tx() {
    if (_tx_packetq.size() < 16) {
        // refill send queue from upper layers
        uint32_t work;
        do {
            work = 0;
            for (auto&& pr : _pkt_providers) {
                auto p = pr();
                if (p) {
                    work++;
                    _tx_packetq.push_back(std::move(p.value()));
                    if (_tx_packetq.size() == 128) {
                        break;
                    }
                }
            }
        } while (work && _tx_packetq.size() < 128);
    }
    if (!_tx_packetq.empty()) {
        _stats.tx.good.update_pkts_bunch(send(_tx_packetq));
        return true;
    }

    return false;
}

qp::qp(bool register_copy_stats,
       const std::string stats_plugin_name, uint8_t qid)
        : _tx_poller(std::make_unique<internal::poller>(reactor::poller::simple([this] { return poll_tx(); })))
        , _stats_plugin_name(stats_plugin_name)
        , _queue_name(std::string("queue") + std::to_string(qid))
{
    namespace sm = metrics;

    _metrics.add_group(_stats_plugin_name, {
        //
        // Packets rate: DERIVE:0:u
        //
        sm::make_counter(_queue_name + "_rx_packets", _stats.rx.good.packets,
                        sm::description("This metric is a receive packet rate for this queue.")),

        sm::make_counter(_queue_name + "_tx_packets", _stats.tx.good.packets,
                        sm::description("This metric is a transmit packet rate for this queue.")),
        //
        // Bytes rate: DERIVE:0:U
        //
        sm::make_counter(_queue_name + "_rx_bytes", _stats.rx.good.bytes,
                        sm::description("This metric is a receive throughput for this queue.")),

        sm::make_counter(_queue_name + "_tx_bytes", _stats.tx.good.bytes,
                        sm::description("This metric is a transmit throughput for this queue.")),
        //
        // Queue length: GAUGE:0:U
        //
        // Tx
        sm::make_gauge(_queue_name + "_tx_packet_queue", [this] { return _tx_packetq.size(); },
                        sm::description("Holds a number of packets pending to be sent. "
                                        "This metric will have high values if the network backend doesn't keep up with the upper layers or if upper layers send big bursts of packets.")),

        //
        // Linearization counter: DERIVE:0:U
        //
        sm::make_counter(_queue_name + "_xmit_linearized", _stats.tx.linearized,
                        sm::description("Counts a number of linearized Tx packets. High value indicates that we send too fragmented packets.")),

        //
        // Number of packets in last bunch: GAUGE:0:U
        //
        // Tx
        sm::make_gauge(_queue_name + "_tx_packet_queue_last_bunch", _stats.tx.good.last_bunch,
                        sm::description(seastar::format("Holds a number of packets sent in the bunch. "
                                        "A high value in conjunction with a high value of a {} indicates an efficient Tx packets bulking.", _queue_name + "_tx_packet_queue"))),
        // Rx
        sm::make_gauge(_queue_name + "_rx_packet_queue_last_bunch", _stats.rx.good.last_bunch,
                        sm::description("Holds a number of packets received in the last Rx bunch. High value indicates an efficient Rx packets bulking.")),

        //
        // Fragments rate: DERIVE:0:U
        //
        // Tx
        sm::make_counter(_queue_name + "_tx_frags", _stats.tx.good.nr_frags,
                        sm::description(seastar::format("Counts a number of sent fragments. Divide this value by a {} to get an average number of fragments in a Tx packet.", _queue_name + "_tx_packets"))),
        // Rx
        sm::make_counter(_queue_name + "_rx_frags", _stats.rx.good.nr_frags,
                        sm::description(seastar::format("Counts a number of received fragments. Divide this value by a {} to get an average number of fragments in an Rx packet.", _queue_name + "_rx_packets"))),
    });

    if (register_copy_stats) {
        _metrics.add_group(_stats_plugin_name, {
            //
            // Non-zero-copy data bytes rate: DERIVE:0:u
            //
            // Tx
            sm::make_counter(_queue_name + "_tx_copy_bytes", _stats.tx.good.copy_bytes,
                        sm::description(seastar::format("Counts a number of sent bytes that were handled in a non-zero-copy way. Divide this value by a {} to get a portion of data sent using a non-zero-copy flow.", _queue_name + "_tx_bytes"))),
            // Rx
            sm::make_counter(_queue_name + "_rx_copy_bytes", _stats.rx.good.copy_bytes,
                        sm::description(seastar::format("Counts a number of received bytes that were handled in a non-zero-copy way. Divide this value by an {} to get a portion of received data handled using a non-zero-copy flow.", _queue_name + "_rx_bytes"))),

            //
            // Non-zero-copy data fragments rate: DERIVE:0:u
            //
            // Tx
            sm::make_counter(_queue_name + "_tx_copy_frags", _stats.tx.good.copy_frags,
                        sm::description(seastar::format("Counts a number of sent fragments that were handled in a non-zero-copy way. Divide this value by a {} to get a portion of fragments sent using a non-zero-copy flow.", _queue_name + "_tx_frags"))),
            // Rx
            sm::make_counter(_queue_name + "_rx_copy_frags", _stats.rx.good.copy_frags,
                        sm::description(seastar::format("Counts a number of received fragments that were handled in a non-zero-copy way. Divide this value by a {} to get a portion of received fragments handled using a non-zero-copy flow.", _queue_name + "_rx_frags"))),

        });
    }
}

qp::~qp() {
}

void qp::configure_proxies(const std::map<unsigned, float>& cpu_weights) {
    SEASTAR_ASSERT(!cpu_weights.empty());
    if ((cpu_weights.size() == 1 && cpu_weights.begin()->first == this_shard_id())) {
        // special case queue sending to self only, to avoid requiring a hash value
        return;
    }
    register_packet_provider([this] {
        std::optional<packet> p;
        if (!_proxy_packetq.empty()) {
            p = std::move(_proxy_packetq.front());
            _proxy_packetq.pop_front();
        }
        return p;
    });
    build_sw_reta(cpu_weights);
}

void qp::build_sw_reta(const std::map<unsigned, float>& cpu_weights) {
    float total_weight = 0;
    for (auto&& x : cpu_weights) {
        total_weight += x.second;
    }
    float accum = 0;
    unsigned idx = 0;
    std::array<uint8_t, 128> reta;
    for (auto&& entry : cpu_weights) {
        auto cpu = entry.first;
        auto weight = entry.second;
        accum += weight;
        while (idx < (accum / total_weight * reta.size() - 0.5)) {
            reta[idx++] = cpu;
        }
    }
    _sw_reta = reta;
}

future<>
device::receive(std::function<future<> (packet)> next_packet) {
    auto sub = _queues[this_shard_id()]->_rx_stream.listen(std::move(next_packet));
    _queues[this_shard_id()]->rx_start();
    return sub.done();
}

void device::set_local_queue(std::unique_ptr<qp> dev) {
    SEASTAR_ASSERT(!_queues[this_shard_id()]);
    _queues[this_shard_id()] = dev.get();
    engine().at_destroy([dev = std::move(dev)] {});
}


l3_protocol::l3_protocol(interface* netif, eth_protocol_num proto_num, packet_provider_type func)
    : _netif(netif), _proto_num(proto_num)  {
        _netif->register_packet_provider(std::move(func));
}

future<> l3_protocol::receive(
        std::function<future<> (packet p, ethernet_address from)> rx_fn,
        std::function<bool (forward_hash&, packet&, size_t)> forward) {
    return _netif->register_l3(_proto_num, std::move(rx_fn), std::move(forward));
};

interface::interface(std::shared_ptr<device> dev)
    : _dev(dev)
    , _hw_address(_dev->hw_address())
    , _hw_features(_dev->hw_features()) {
    // FIXME: ignored future
    (void)_dev->receive([this] (packet p) {
        return dispatch_packet(std::move(p));
    });
    dev->local_queue().register_packet_provider([this, idx = 0u] () mutable {
            std::optional<packet> p;
            for (size_t i = 0; i < _pkt_providers.size(); i++) {
                auto l3p = _pkt_providers[idx++]();
                if (idx == _pkt_providers.size())
                    idx = 0;
                if (l3p) {
                    auto l3pv = std::move(l3p.value());
                    auto eh = l3pv.p.prepend_header<eth_hdr>();
                    eh->dst_mac = l3pv.to;
                    eh->src_mac = _hw_address;
                    eh->eth_proto = uint16_t(l3pv.proto_num);
                    *eh = hton(*eh);
                    p = std::move(l3pv.p);
                    return p;
                }
            }
            return p;
        });
}

future<>
interface::register_l3(eth_protocol_num proto_num,
        std::function<future<> (packet p, ethernet_address from)> next,
        std::function<bool (forward_hash&, packet& p, size_t)> forward) {
    auto i = _proto_map.emplace(std::piecewise_construct, std::make_tuple(uint16_t(proto_num)), std::forward_as_tuple(std::move(forward)));
    SEASTAR_ASSERT(i.second);
    l3_rx_stream& l3_rx = i.first->second;
    return l3_rx.packet_stream.listen(std::move(next)).done();
}

unsigned interface::hash2cpu(uint32_t hash) {
    return _dev->hash2cpu(hash);
}

uint16_t interface::hw_queues_count() {
    return _dev->hw_queues_count();
}

rss_key_type interface::rss_key() const {
    return _dev->rss_key();
}

void interface::forward(unsigned cpuid, packet p) {
    static __thread unsigned queue_depth;

    if (queue_depth < 1000) {
        queue_depth++;
        auto src_cpu = this_shard_id();
        // FIXME: future is discarded
        (void)smp::submit_to(cpuid, [this, p = std::move(p), src_cpu]() mutable {
            _dev->l2receive(p.free_on_cpu(src_cpu));
        }).then([] {
            queue_depth--;
        });
    }
}

future<> interface::dispatch_packet(packet p) {
    auto eh = p.get_header<eth_hdr>();
    if (eh) {
        auto i = _proto_map.find(ntoh(eh->eth_proto));
        if (i != _proto_map.end()) {
            l3_rx_stream& l3 = i->second;
            auto fw = _dev->forward_dst(this_shard_id(), [&p, &l3, this] () {
                auto hwrss = p.rss_hash();
                if (hwrss) {
                    return hwrss.value();
                } else {
                    forward_hash data;
                    if (l3.forward(data, p, sizeof(eth_hdr))) {
                        return toeplitz_hash(rss_key(), data);
                    }
                    return 0u;
                }
            });
            if (fw != this_shard_id()) {
                forward(fw, std::move(p));
            } else {
                auto h = ntoh(*eh);
                auto from = h.src_mac;
                p.trim_front(sizeof(*eh));
                // avoid chaining, since queue lenth is unlimited
                // drop instead.
                if (l3.ready.available()) {
                    l3.ready = l3.packet_stream.produce(std::move(p), from);
                }
            }
        }
    }
    return make_ready_future<>();
}

}

}
