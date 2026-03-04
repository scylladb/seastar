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

// Copyright (C) 2025-present ScyllaDB

#include <seastar/core/thread.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/when_all.hh>
#include <seastar/net/api.hh>

#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

#include <vector>

// Comprehensive tests for proxy protocol v2 implementation

using namespace seastar;

// Helper function to create a valid proxy protocol v2 header
static std::vector<char> make_proxy_v2_header(
    bool valid_signature = true,
    uint8_t version_cmd = 0x21,  // v2 PROXY
    uint8_t family_proto = 0x11, // IPv4 TCP
    socket_address src_addr = ipv4_addr("192.168.1.100", 5000),
    socket_address dst_addr = ipv4_addr("10.0.0.1", 8080),
    std::vector<char> tlvs = {}) {

    std::vector<char> header;

    // Signature (12 bytes)
    if (valid_signature) {
        const char sig[12] = {0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a};
        header.insert(header.end(), sig, sig + 12);
    } else {
        const char bad_sig[12] = {0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0b};
        header.insert(header.end(), bad_sig, bad_sig + 12);
    }

    // Version and command
    header.push_back(version_cmd);

    // Family and protocol
    header.push_back(family_proto);

    // Length (will be filled in later)
    size_t len_pos = header.size();
    header.push_back(0);
    header.push_back(0);

    // Address information
    uint16_t addr_len = 0;
    if ((version_cmd & 0x0F) == 0x01) { // PROXY command
        if (family_proto == 0x11) { // IPv4 TCP
            addr_len = 12;
            auto src_in = src_addr.as_posix_sockaddr_in();
            auto dst_in = dst_addr.as_posix_sockaddr_in();

            // Source address
            header.push_back((src_in.sin_addr.s_addr >> 0) & 0xFF);
            header.push_back((src_in.sin_addr.s_addr >> 8) & 0xFF);
            header.push_back((src_in.sin_addr.s_addr >> 16) & 0xFF);
            header.push_back((src_in.sin_addr.s_addr >> 24) & 0xFF);

            // Destination address
            header.push_back((dst_in.sin_addr.s_addr >> 0) & 0xFF);
            header.push_back((dst_in.sin_addr.s_addr >> 8) & 0xFF);
            header.push_back((dst_in.sin_addr.s_addr >> 16) & 0xFF);
            header.push_back((dst_in.sin_addr.s_addr >> 24) & 0xFF);

            // Source port
            uint16_t src_port = ntohs(src_in.sin_port);
            header.push_back((src_port >> 8) & 0xFF);
            header.push_back((src_port >> 0) & 0xFF);

            // Destination port
            uint16_t dst_port = ntohs(dst_in.sin_port);
            header.push_back((dst_port >> 8) & 0xFF);
            header.push_back((dst_port >> 0) & 0xFF);
        } else if (family_proto == 0x21) { // IPv6 TCP
            addr_len = 36;
            auto src_in6 = src_addr.as_posix_sockaddr_in6();
            auto dst_in6 = dst_addr.as_posix_sockaddr_in6();

            // Source address (16 bytes)
            header.insert(header.end(),
                reinterpret_cast<const char*>(&src_in6.sin6_addr),
                reinterpret_cast<const char*>(&src_in6.sin6_addr) + 16);

            // Destination address (16 bytes)
            header.insert(header.end(),
                reinterpret_cast<const char*>(&dst_in6.sin6_addr),
                reinterpret_cast<const char*>(&dst_in6.sin6_addr) + 16);

            // Source port
            uint16_t src_port = ntohs(src_in6.sin6_port);
            header.push_back((src_port >> 8) & 0xFF);
            header.push_back((src_port >> 0) & 0xFF);

            // Destination port
            uint16_t dst_port = ntohs(dst_in6.sin6_port);
            header.push_back((dst_port >> 8) & 0xFF);
            header.push_back((dst_port >> 0) & 0xFF);
        }
    } else if ((version_cmd & 0x0F) == 0x00) { // LOCAL command
        addr_len = 0;  // No address data for LOCAL
    }

    // Add TLVs
    header.insert(header.end(), tlvs.begin(), tlvs.end());
    addr_len += tlvs.size();

    // Fill in length
    header[len_pos] = (addr_len >> 8) & 0xFF;
    header[len_pos + 1] = addr_len & 0xFF;

    return header;
}

struct proxy_test_result {
    bool got_connection = false;
    bool timed_out = false;
};

struct server_addresses {
    socket_address remote;
    socket_address local;
};

// Helper to run server with timeout - returns result
static future<proxy_test_result> run_server(server_socket& ss) {
    return seastar::async([&ss] {
        using namespace std::chrono_literals;

        proxy_test_result result;

        timer<> abort_timer([&ss, &result] {
            result.timed_out = true;
            ss.abort_accept();
        });
        abort_timer.arm(timer<>::clock::now() + 100ms);

        try {
            auto ar = ss.accept().get();
            result.got_connection = true;
            ar.connection.shutdown_output();
        } catch (...) {
            // Expected - abort_accept throws or connection rejected
        }

        return result;
    });
}

// Helper to run server for positive tests - accepts connection and returns addresses
static future<server_addresses> run_server_accept(server_socket& ss) {
    return seastar::async([&ss] {
        auto ar = ss.accept().get();
        server_addresses addrs;
        addrs.remote = ar.connection.remote_address();
        addrs.local = ar.connection.local_address();
        ar.connection.shutdown_output();
        return addrs;
    });
}

// Helper function for negative tests - expects connection to be rejected
static void test_proxy_header_negative(
    socket_address listen_addr,
    std::vector<char> header) {

    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    server_socket ss = seastar::listen(listen_addr, lo);

    auto server = run_server(ss);

    auto client = seastar::async([&listen_addr, header = std::move(header)] {
        try {
            auto s = connect(listen_addr).get();
            auto out = s.output();
            out.write(header.data(), header.size()).get();
            out.flush().get();
            out.close().get();
        } catch (...) {
            // Expected - server may close connection during write/flush/close
        }
    });

    auto [result] = when_all_succeed(std::move(server), std::move(client)).get();
    ss.abort_accept();

    BOOST_REQUIRE(!result.got_connection);
    BOOST_REQUIRE(result.timed_out);
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_bad_signature) {
    // Invalid signature should cause connection to be dropped
    auto header = make_proxy_v2_header(false);  // bad signature
    test_proxy_header_negative(ipv4_addr("127.0.0.1", 12001), std::move(header));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_small_packet_incomplete_header) {
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv4_addr("127.0.0.1", 12002);
    server_socket ss = seastar::listen(listen_addr, lo);

    auto server = run_server(ss);

    // Send only 10 bytes (header needs 16)
    auto client = seastar::async([&listen_addr] {
        try {
            auto s = connect(listen_addr).get();
            auto out = s.output();
            char partial[10] = {0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49};
            out.write(partial, sizeof(partial)).get();
            out.flush().get();
            out.close().get();
        } catch (...) {
            // ignore
        }
    });

    auto [result] = when_all_succeed(std::move(server), std::move(client)).get();
    ss.abort_accept();

    BOOST_REQUIRE(!result.got_connection);
    BOOST_REQUIRE(result.timed_out);
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_small_packet_incomplete_addresses) {
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv4_addr("127.0.0.1", 12003);
    server_socket ss = seastar::listen(listen_addr, lo);

    auto server = run_server(ss);

    // Send header claiming 12 bytes of addresses but only send 6
    auto client = seastar::async([&listen_addr] {
        try {
            auto s = connect(listen_addr).get();
            auto out = s.output();
            unsigned char buf[22];
            const unsigned char sig[12] = {0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a};
            std::memcpy(buf, sig, 12);
            buf[12] = 0x21; // v2 PROXY
            buf[13] = 0x11; // IPv4 TCP
            buf[14] = 0x00; // length high byte
            buf[15] = 0x0C; // length low byte (12 bytes)
            // Only send 6 bytes of address data
            buf[16] = 192;
            buf[17] = 168;
            buf[18] = 1;
            buf[19] = 100;
            buf[20] = 10;
            buf[21] = 0;
            out.write(reinterpret_cast<char*>(buf), sizeof(buf)).get();
            out.flush().get();
            out.close().get();
        } catch (...) {
            // ignore
        }
    });

    auto [result] = when_all_succeed(std::move(server), std::move(client)).get();
    ss.abort_accept();

    BOOST_REQUIRE(!result.got_connection);
    BOOST_REQUIRE(result.timed_out);
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_local_mode) {
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv4_addr("127.0.0.1", 12004);
    server_socket ss = seastar::listen(listen_addr, lo);

    auto server = run_server_accept(ss);

    // Send LOCAL command with UNSPEC family
    auto header = make_proxy_v2_header(true, 0x20, 0x00);  // v2 LOCAL, UNSPEC
    socket_address client_actual_local;
    auto client = seastar::async([&listen_addr, &client_actual_local, header = std::move(header)] {
        auto s = connect(listen_addr).get();
        client_actual_local = s.local_address();
        auto out = s.output();
        out.write(header.data(), header.size()).get();
        out.flush().get();
        out.close().get();
    });

    auto [addrs] = when_all_succeed(std::move(server), std::move(client)).get();
    ss.abort_accept();

    // With LOCAL mode, addresses should be real connection addresses, not from header
    // We can't check exact match since the client port is ephemeral, but we can verify
    // that the addresses are not the fake proxy addresses and the IPs match
    BOOST_REQUIRE_EQUAL(addrs.local, socket_address(listen_addr));
    BOOST_REQUIRE_EQUAL(addrs.remote, client_actual_local);
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_local_mode_wrong_family) {
    // LOCAL command must use UNSPEC family (0x00), not other families
    auto header = make_proxy_v2_header(true, 0x20, 0x11);  // v2 LOCAL, but IPv4 TCP (invalid)
    test_proxy_header_negative(ipv4_addr("127.0.0.1", 12005), std::move(header));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_ipv4_addresses) {
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv4_addr("127.0.0.1", 12006);
    server_socket ss = seastar::listen(listen_addr, lo);

    auto expected_remote = ipv4_addr("192.168.1.100", 5000);
    auto expected_local = ipv4_addr("10.0.0.1", 8080);

    auto server = run_server_accept(ss);

    auto header = make_proxy_v2_header(true, 0x21, 0x11, expected_remote, expected_local);
    auto client = seastar::async([&listen_addr, header = std::move(header)] {
        auto s = connect(listen_addr).get();
        auto out = s.output();
        out.write(header.data(), header.size()).get();
        out.flush().get();
        out.close().get();
    });

    auto [addrs] = when_all_succeed(std::move(server), std::move(client)).get();
    ss.abort_accept();

    // Verify addresses match what was sent in the header
    BOOST_REQUIRE_EQUAL(addrs.remote, socket_address(expected_remote));
    BOOST_REQUIRE_EQUAL(addrs.local, socket_address(expected_local));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_ipv6_addresses) {
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv6_addr("::1", 12007);
    server_socket ss = seastar::listen(listen_addr, lo);

    auto expected_remote = ipv6_addr("2001:db8::1", 5000);
    auto expected_local = ipv6_addr("2001:db8::2", 8080);

    auto server = run_server_accept(ss);

    auto header = make_proxy_v2_header(true, 0x21, 0x21, expected_remote, expected_local);
    auto client = seastar::async([&listen_addr, header = std::move(header)] {
        auto s = connect(listen_addr).get();
        auto out = s.output();
        out.write(header.data(), header.size()).get();
        out.flush().get();
        out.close().get();
    });

    auto [addrs] = when_all_succeed(std::move(server), std::move(client)).get();
    ss.abort_accept();

    // Verify addresses match what was sent in the header
    BOOST_REQUIRE_EQUAL(addrs.remote, socket_address(expected_remote));
    BOOST_REQUIRE_EQUAL(addrs.local, socket_address(expected_local));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_invalid_version) {
    // Only version 2 (0x2X) is supported
    auto header = make_proxy_v2_header(true, 0x11);  // version 1 (invalid for v2 format)
    test_proxy_header_negative(ipv4_addr("127.0.0.1", 12008), std::move(header));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_invalid_command) {
    // Only LOCAL (0x0) and PROXY (0x1) commands are valid
    auto header = make_proxy_v2_header(true, 0x22);  // v2 with invalid command (0x2)
    test_proxy_header_negative(ipv4_addr("127.0.0.1", 12009), std::move(header));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_unsupported_family) {
    // Only AF_INET (0x1) and AF_INET6 (0x2) are supported for PROXY command
    auto header = make_proxy_v2_header(true, 0x21, 0x31);  // v2 PROXY, AF_UNIX (not supported)
    test_proxy_header_negative(ipv4_addr("127.0.0.1", 12010), std::move(header));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_unsupported_protocol) {
    // Only STREAM (0x1) is supported
    auto header = make_proxy_v2_header(true, 0x21, 0x12);  // v2 PROXY, IPv4 DGRAM (not supported)
    test_proxy_header_negative(ipv4_addr("127.0.0.1", 12011), std::move(header));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_length_too_short_ipv4) {
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv4_addr("127.0.0.1", 12012);
    server_socket ss = seastar::listen(listen_addr, lo);

    auto server = run_server(ss);

    // IPv4 requires 12 bytes, send header claiming only 8
    auto client = seastar::async([&listen_addr] {
        try {
            auto s = connect(listen_addr).get();
            auto out = s.output();
            unsigned char buf[24];
            const unsigned char sig[12] = {0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a};
            std::memcpy(buf, sig, 12);
            buf[12] = 0x21; // v2 PROXY
            buf[13] = 0x11; // IPv4 TCP
            buf[14] = 0x00; // length high byte
            buf[15] = 0x08; // length low byte (8 bytes, too short)
            // Fill in 8 bytes of data
            for (int i = 16; i < 24; ++i) {
                buf[i] = 0;
            }
            out.write(reinterpret_cast<char*>(buf), sizeof(buf)).get();
            out.flush().get();
            out.close().get();
        } catch (...) {
            // ignore
        }
    });

    auto [result] = when_all_succeed(std::move(server), std::move(client)).get();
    ss.abort_accept();

    BOOST_REQUIRE(!result.got_connection);
    BOOST_REQUIRE(result.timed_out);
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_length_too_short_ipv6) {
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv6_addr("::1", 12013);
    server_socket ss = seastar::listen(listen_addr, lo);

    auto server = run_server(ss);

    // IPv6 requires 36 bytes, send header claiming only 20
    auto client = seastar::async([&listen_addr] {
        try {
            auto s = connect(listen_addr).get();
            auto out = s.output();
            std::vector<unsigned char> buf(36);
            const unsigned char sig[12] = {0x0d, 0x0a, 0x0d, 0x0a, 0x00, 0x0d, 0x0a, 0x51, 0x55, 0x49, 0x54, 0x0a};
            std::memcpy(buf.data(), sig, 12);
            buf[12] = 0x21; // v2 PROXY
            buf[13] = 0x21; // IPv6 TCP
            buf[14] = 0x00; // length high byte
            buf[15] = 0x14; // length low byte (20 bytes, too short)
            // Fill in 20 bytes of data
            out.write(reinterpret_cast<char*>(buf.data()), 36).get();
            out.flush().get();
            out.close().get();
        } catch (...) {
            // ignore
        }
    });

    auto [result] = when_all_succeed(std::move(server), std::move(client)).get();
    ss.abort_accept();

    BOOST_REQUIRE(!result.got_connection);
    BOOST_REQUIRE(result.timed_out);
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_with_tlvs) {
    // TLVs should be skipped/ignored by the implementation
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv4_addr("127.0.0.1", 12014);
    server_socket ss = seastar::listen(listen_addr, lo);

    auto expected_remote = ipv4_addr("192.168.1.100", 5000);
    auto expected_local = ipv4_addr("10.0.0.1", 8080);

    auto server = run_server_accept(ss);

    // Add some TLVs (type, length_hi, length_lo, value...)
    std::vector<char> tlvs;
    tlvs.push_back(0x01); // PP2_TYPE_ALPN
    tlvs.push_back(0x00); // length high
    tlvs.push_back(0x08); // length low (8 bytes)
    tlvs.insert(tlvs.end(), 8, 'x'); // dummy data

    auto header = make_proxy_v2_header(true, 0x21, 0x11, expected_remote, expected_local, tlvs);
    auto client = seastar::async([&listen_addr, header = std::move(header)] {
        auto s = connect(listen_addr).get();
        auto out = s.output();
        out.write(header.data(), header.size()).get();
        out.flush().get();
        out.close().get();
    });

    auto [addrs] = when_all_succeed(std::move(server), std::move(client)).get();
    ss.abort_accept();

    // Verify addresses still work correctly despite TLVs
    BOOST_REQUIRE_EQUAL(addrs.remote, socket_address(expected_remote));
    BOOST_REQUIRE_EQUAL(addrs.local, socket_address(expected_local));
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_extreme_ports) {
    // Test with port 0 and port 65535
    listen_options lo;
    lo.reuse_address = true;
    lo.proxy_protocol = true;

    auto listen_addr = ipv4_addr("127.0.0.1", 12015);
    server_socket ss = seastar::listen(listen_addr, lo);

    auto expected_remote = ipv4_addr("192.168.1.100", 0);      // port 0
    auto expected_local = ipv4_addr("10.0.0.1", 65535);        // port 65535

    auto server = run_server_accept(ss);

    auto header = make_proxy_v2_header(true, 0x21, 0x11, expected_remote, expected_local);
    auto client = seastar::async([&listen_addr, header = std::move(header)] {
        auto s = connect(listen_addr).get();
        auto out = s.output();
        out.write(header.data(), header.size()).get();
        out.flush().get();
        out.close().get();
    });

    auto [addrs] = when_all_succeed(std::move(server), std::move(client)).get();
    ss.abort_accept();

    BOOST_REQUIRE_EQUAL(addrs.remote.port(), 0);
    BOOST_REQUIRE_EQUAL(addrs.local.port(), 65535);
}

SEASTAR_THREAD_TEST_CASE(proxy_protocol_v2_unspec_family) {
    // UNSPEC family (0x0) should only be valid with LOCAL command
    auto header = make_proxy_v2_header(true, 0x21, 0x00);  // v2 PROXY with UNSPEC (invalid combo)
    test_proxy_header_negative(ipv4_addr("127.0.0.1", 12016), std::move(header));
}
