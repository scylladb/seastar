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
 * Copyright (C) 2019 Cloudius Systems, Ltd.
 */

#include <seastar/testing/test_case.hh>
#include <seastar/net/api.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/log.hh>

using namespace seastar;

static logger iplog("ipv6");

static bool check_ipv6_support() {
    if (!engine().net().supports_ipv6()) {
        iplog.info("No IPV6 support detected. Skipping...");
        return false;
    }
    return true;
}

SEASTAR_TEST_CASE(udp_packet_test) {
    if (!check_ipv6_support()) {
        return make_ready_future<>();
    }

    auto sc = make_bound_datagram_channel(ipv6_addr{"::1"});

    BOOST_REQUIRE(sc.local_address().addr().is_ipv6());

    auto cc = make_bound_datagram_channel(ipv6_addr{"::1"});

    auto f1 = cc.send(sc.local_address(), "apa");

    return f1.then([cc = std::move(cc), sc = std::move(sc)]() mutable {
        auto src = cc.local_address();
        cc.close();
        auto f2 = sc.receive();

        return f2.then([sc = std::move(sc), src](auto pkt) mutable {
            auto a = sc.local_address();
            sc.close();
            BOOST_REQUIRE_EQUAL(src, pkt.get_src());
            auto dst = pkt.get_dst();
            // Don't always get a dst address.
            if (dst != socket_address()) {
                BOOST_REQUIRE_EQUAL(a, pkt.get_dst());
            }
        });
    });
}

SEASTAR_TEST_CASE(tcp_packet_test) {
    if (!check_ipv6_support()) {
        return make_ready_future<>();
    }

    return async([] {
        auto sc = server_socket(engine().net().listen(ipv6_addr{"::1"}, {}));
        auto la = sc.local_address();

        BOOST_REQUIRE(la.addr().is_ipv6());

        auto cc = connect(la).get();
        auto lc = std::move(sc.accept().get().connection);

        auto strm = cc.output();
        strm.write("los lobos").get();
        strm.flush().get();

        auto in = lc.input();

        using consumption_result_type = typename input_stream<char>::consumption_result_type;
        using stop_consuming_type = typename consumption_result_type::stop_consuming_type;
        using tmp_buf = stop_consuming_type::tmp_buf;

        in.consume([](tmp_buf buf) {
            return make_ready_future<consumption_result_type>(stop_consuming<char>({}));
        }).get();

        strm.close().get();
        in.close().get();
        sc.abort_accept();
    });
}

SEASTAR_TEST_CASE(ipv6_equal_test) {
    const uint16_t port{8080};
    const uint16_t port2{8088};

    const std::string str_addr1{"abcd:fedc:ba98:7654:3210:0123:4567:89ab"};
    const std::string str_addr2{"0123:4567:89ab:cdef:3210:0123:4567:89ab"};
    const std::string str_addr3{"abcd:fedc:ba98:7654:3210:0123:4567:8900"};

    socket_address sock_addr1(ipv6_addr(str_addr1, port));
    socket_address sock_addr2(ipv6_addr(str_addr2, port));
    socket_address sock_addr3(ipv6_addr(str_addr1, port));

    socket_address sock_addr4(ipv6_addr(str_addr3, port));
    socket_address sock_addr5(ipv6_addr(str_addr1, port2));

    BOOST_CHECK_NE(sock_addr1, sock_addr2);
    BOOST_CHECK_EQUAL(sock_addr1, sock_addr3);
    BOOST_CHECK_NE(sock_addr1, sock_addr4);
    BOOST_CHECK_NE(sock_addr1, sock_addr5);

    return make_ready_future();
}

