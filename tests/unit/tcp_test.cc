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
 * Copyright (C) 2026 ScyllaDB Ltd.
 */

#include <seastar/testing/test_case.hh>
#include <seastar/net/tcp.hh>
#include <seastar/net/ip.hh>
#include <seastar/util/later.hh>

using namespace seastar;
using namespace net;

namespace {

// Minimal stand-in for ipv4_l4<tcp>, enough to feed segments into tcp<>.
struct mock_ipv4 {
    net::hw_features _hw;
    const net::hw_features& hw_features() const { return _hw; }
    ipv4_address host_address() const { return ipv4_address(0x0a000001); }
};

struct mock_inet {
    mock_ipv4 _inet;
    ipv4_traits::packet_provider_type _provider;
    void register_packet_provider(ipv4_traits::packet_provider_type p) {
        _provider = std::move(p);
    }
    future<ethernet_address> get_l2_dst_address(ipv4_address) {
        return make_ready_future<ethernet_address>();
    }
};

struct mock_traits : ipv4_traits {
    using inet_type = mock_inet;
};

packet make_segment(uint8_t data_offset, uint16_t dst_port) {
    packet p;
    auto th = p.prepend_uninitialized_header(tcp_hdr::len);
    auto h = tcp_hdr{};
    h.src_port = 12345;
    h.dst_port = dst_port;
    h.data_offset = data_offset;
    h.f_syn = true;
    h.window = 65535;
    h.write(th);
    return p;
}

}

SEASTAR_TEST_CASE(test_oversized_data_offset_is_dropped) {
    mock_inet inet;
    inet._inet._hw.rx_csum_offload = true;
    tcp<mock_traits> t(inet);
    constexpr uint16_t port = 10000;
    // With a backlog of one, the listener becomes full as soon as a SYN
    // reaches the listen-state handler.
    auto l = t.listen(port, 1);
    const auto from = ipv4_address(0x0a000002);
    const auto to = ipv4_address(0x0a000001);
    // A 20-byte SYN claiming a 60-byte header must be dropped instead of
    // reaching packet::trim_front() with a length past the end of the packet.
    t.received(make_segment(15, port), from, to);
    BOOST_REQUIRE(!l.full());
    // A well-formed SYN on the same path is accepted.
    t.received(make_segment(5, port), from, to);
    BOOST_REQUIRE(l.full());
    // The SYN-ACK is queued by a continuation that references t; wait until
    // it is handed to the packet provider, so it does not outlive t.
    std::optional<ipv4_traits::l4packet> syn_ack;
    for (int i = 0; i < 1000 && !syn_ack; i++) {
        co_await yield();
        syn_ack = inet._provider();
    }
    BOOST_REQUIRE(syn_ack);
}
