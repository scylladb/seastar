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
 * Copyright (C) 2026 zorjen122
 */

#include <seastar/core/manual_clock.hh>
#include <seastar/net/tcp.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/util/later.hh>

#include <bitset>
#include <memory>
#include <optional>
#include <string_view>

using namespace seastar;
using namespace seastar::net;
using namespace std::chrono_literals;

namespace {

struct wire {
    struct interface {
        rss_key_type rss_key() const { return default_rsskey_40bytes; }
        unsigned hash2cpu(uint32_t hash) { return hash % this_smp_shard_count(); }
    };
    struct ip_layer {
        interface nif;
        net::hw_features features;
        ipv4_address host_address() const { return ipv4_address("10.0.0.1"); }
        interface* netif() { return &nif; }
        const net::hw_features& hw_features() const { return features; }
    } _inet;
    ipv4_traits::packet_provider_type provider;
    void register_packet_provider(ipv4_traits::packet_provider_type p) { provider = std::move(p); }
    future<ethernet_address> get_l2_dst_address(ipv4_address) {
        return make_ready_future<ethernet_address>(ethernet_address{});
    }
};
struct traits : ipv4_traits {
    using inet_type = wire;
    using tcp_clock_type = manual_clock;
};
using protocol = tcp<traits>;
constexpr unsigned FIN = 1, SYN = 2, RST = 4, ACK = 16;
const ipv4_address local("10.0.0.1"), peer("10.0.0.2");

static void inject(protocol& stack, uint16_t port, tcp_seq seq, tcp_seq ack,
        unsigned flags, std::string_view data = {}, std::shared_ptr<bool> released = {}) {
    packet p(data.data(), data.size());
    if (released) {
        p = packet(std::move(p), make_deleter([released] { *released = true; }));
    }
    auto bytes = p.prepend_uninitialized_header(tcp_hdr::len);
    tcp_hdr h{};
    h.src_port = 80;
    h.dst_port = port;
    h.seq = seq;
    h.ack = ack;
    h.f_fin = flags & FIN;
    h.f_syn = bool(flags & SYN);
    h.f_rst = bool(flags & RST);
    h.f_ack = bool(flags & ACK);
    h.data_offset = tcp_hdr::len / 4;
    h.window = 32000;
    h.write(bytes);
    checksummer sum;
    ipv4_traits::tcp_pseudo_header_checksum(sum, peer, local, p.len());
    sum.sum(p);
    tcp_hdr::write_nbo_checksum(bytes, sum.get());
    stack.received(std::move(p), peer, local);
    yield().get();
}

static tcp_hdr take(wire& inet) {
    std::optional<ipv4_traits::l4packet> outgoing;
    auto deadline = steady_clock_type::now() + 5s;
    while (!(outgoing = inet.provider())) {
        BOOST_REQUIRE_MESSAGE(steady_clock_type::now() < deadline, "expected a TCP response");
        yield().get();
    }
    checksummer sum;
    ipv4_traits::tcp_pseudo_header_checksum(sum, local, peer, outgoing->p.len());
    sum.sum(outgoing->p);
    BOOST_REQUIRE_EQUAL(sum.get(), 0);
    auto h = tcp_hdr::read(outgoing->p.get_header(0, tcp_hdr::len));
    BOOST_REQUIRE_EQUAL(outgoing->p.len(), h.data_offset * 4u);
    return h;
}

static void expect_ack(wire& inet, tcp_seq seq, tcp_seq ack) {
    auto h = take(inet);
    BOOST_REQUIRE(h.f_ack && !h.f_syn && !h.f_fin && !h.f_rst);
    BOOST_REQUIRE(h.seq == seq && h.ack == ack);
}

static void advance_to(manual_clock::time_point when) {
    manual_clock::advance(when - manual_clock::now());
    yield().get();
    smp::invoke_on_all([] {}).get();
    yield().get();
}

} // anonymous namespace

SEASTAR_THREAD_TEST_CASE(tcp_time_wait) {
    wire inet;
    protocol stack(inet);
    auto conn = stack.connect(socket_address(ipv4_addr("10.0.0.2", 80)));
    auto port = conn.local_port();
    auto syn = take(inet);
    BOOST_REQUIRE(syn.f_syn && !syn.f_ack);
    auto remote = make_seq(5000);
    inject(stack, port, remote, syn.seq + 1, SYN | ACK);
    conn.connected().get();
    remote += 1;
    expect_ack(inet, syn.seq + 1, remote);

    conn.close_write();
    auto fin = take(inet);
    BOOST_REQUIRE(fin.f_fin && fin.f_ack);
    auto next = fin.seq + 1;
    inject(stack, port, remote, next, ACK);
    inject(stack, port, remote, next, FIN | ACK, "abc");
    remote += 3 + 1;
    // Advance RCV.NXT before consuming the ACK queued on TIME_WAIT entry.
    inject(stack, port, remote, next, FIN | ACK);
    auto original_fin = remote;
    remote += 1;
    expect_ack(inet, next, remote);
    auto started = manual_clock::now();

    inject(stack, port, remote + 1, next, RST | ACK);
    expect_ack(inet, next, remote);
    inject(stack, port, remote, next, SYN | ACK);
    expect_ack(inet, next, remote);

    advance_to(started + 10s);
    inject(stack, port, original_fin, next, FIN | ACK);
    expect_ack(inet, next, remote);

    // At 65s the original 60s deadline has passed, but the renewed one has not.
    advance_to(started + 65s);
    inject(stack, port, remote, next + 1, ACK);
    expect_ack(inet, next, remote);

    // A FIN at RCV.NXT advances it once and renews TIME_WAIT until 125s.
    inject(stack, port, remote, next, FIN | ACK);
    remote += 1;
    expect_ack(inet, next, remote);

    advance_to(started + 75s);
    inject(stack, port, remote, next + 1, ACK);
    expect_ack(inet, next, remote);

    // A stale ACK must neither advance RCV.NXT nor renew the 125s deadline.
    inject(stack, port, remote - 1, next - 1, FIN | ACK);
    expect_ack(inet, next, remote);
    inject(stack, port, remote, next - 1, FIN | ACK);
    expect_ack(inet, next, remote);

    advance_to(started + 126s);
    inject(stack, port, remote, next + 1, ACK);
    BOOST_REQUIRE(take(inet).f_rst);
    auto data = conn.read();
    BOOST_REQUIRE_EQUAL(data.len(), 3u);
    BOOST_REQUIRE_EQUAL(std::string_view(data.get_header(0, 3), 3), "abc");
    BOOST_REQUIRE_EQUAL(conn.read().len(), 0u);
    yield().get();
    BOOST_REQUIRE(!inet.provider());
}

SEASTAR_THREAD_TEST_CASE(tcp_time_wait_releases_unread_data) {
    for (bool destroy_before_time_wait : {false, true}) {
        wire inet;
        protocol stack(inet);
        std::optional<protocol::connection> conn(stack.connect(socket_address(ipv4_addr("10.0.0.2", 80))));
        auto port = conn->local_port();
        auto syn = take(inet);
        auto remote = make_seq(5000);
        inject(stack, port, remote, syn.seq + 1, SYN | ACK);
        conn->connected().get();
        remote += 1;
        expect_ack(inet, syn.seq + 1, remote);
        conn->close_write();
        if (destroy_before_time_wait) {
            conn.reset();
        }
        auto fin = take(inet);
        BOOST_REQUIRE(fin.f_fin);
        auto next = fin.seq + 1;
        auto released = std::make_shared<bool>(false);
        inject(stack, port, remote, next, ACK | FIN, "abc", released);
        remote += 4;
        expect_ack(inet, next, remote);
        BOOST_REQUIRE_EQUAL(*released, destroy_before_time_wait);
        conn.reset();
        BOOST_REQUIRE(*released);

        // Release the retained TCB before destroying the test stack.
        advance_to(manual_clock::now() + 61s);
    }
}

SEASTAR_THREAD_TEST_CASE(tcp_time_wait_port_exhaustion) {
    wire inet;
    protocol stack(inet);
    std::bitset<65536> used;
    unsigned eligible = 0;
    for (unsigned port = 41952; port <= 65535; ++port) {
        auto id = protocol::connid{local, peer, uint16_t(port), 80};
        if (inet._inet.nif.hash2cpu(id.hash(inet._inet.nif.rss_key())) == this_shard_id()) {
            ++eligible;
        }
    }
    BOOST_REQUIRE(eligible > 0);
    auto destination = socket_address(ipv4_addr("10.0.0.2", 80));
    for (unsigned i = 0; i < eligible; ++i) {
        auto conn = stack.connect(destination);
        auto port = conn.local_port();
        BOOST_REQUIRE(!used.test(port));
        used.set(port);
        auto syn = take(inet);
        auto remote = make_seq(5000);
        inject(stack, port, remote, syn.seq + 1, SYN | ACK);
        conn.connected().get();
        remote += 1;
        expect_ack(inet, syn.seq + 1, remote);
        conn.close_write();
        auto fin = take(inet);
        inject(stack, port, remote, fin.seq + 1, ACK | FIN);
        expect_ack(inet, fin.seq + 1, remote + 1);
    }
    BOOST_REQUIRE_EXCEPTION(stack.connect(destination), std::system_error,
            [] (const std::system_error& e) { return e.code().value() == EADDRNOTAVAIL; });
    advance_to(manual_clock::now() + 61s);
    // Expired tuples are available again, and replies reach the new TCB.
    auto conn = stack.connect(destination);
    auto port = conn.local_port();
    BOOST_REQUIRE(used.test(port));
    auto syn = take(inet);
    inject(stack, port, make_seq(7000), syn.seq + 1, SYN | ACK);
    conn.connected().get();
    expect_ack(inet, syn.seq + 1, make_seq(7001));
    inject(stack, port, make_seq(7001), syn.seq + 1, RST | ACK);
}
