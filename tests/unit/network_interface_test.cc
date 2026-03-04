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
#include <seastar/net/ethernet.hh>
#include <seastar/net/ip.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/log.hh>

using namespace seastar;

static logger niflog("network_interface_test");

static_assert(std::is_nothrow_default_constructible_v<net::ethernet_address>);
static_assert(std::is_nothrow_copy_constructible_v<net::ethernet_address>);
static_assert(std::is_nothrow_move_constructible_v<net::ethernet_address>);

SEASTAR_TEST_CASE(list_interfaces) {
    // just verifying we have something. And can access all the stuff.
    auto interfaces = engine().net().network_interfaces();
    BOOST_REQUIRE_GT(interfaces.size(), 0);

    for (auto& nif : interfaces) {
        niflog.info("Iface: {}, index = {}, mtu = {}, loopback = {}, virtual = {}, up = {}",
            nif.name(), nif.index(), nif.mtu(), nif.is_loopback(), nif.is_virtual(), nif.is_up()
        );
        if (nif.hardware_address().size() >= 6) {
            niflog.info("   HW: {}", net::ethernet_address(nif.hardware_address().data()));
        }
        for (auto& addr : nif.addresses()) {
            niflog.info("   Addr: {}", addr);
        }
    }

    return make_ready_future();
}

SEASTAR_TEST_CASE(match_ipv6_scope) {
    auto interfaces = engine().net().network_interfaces();

    for (auto& nif : interfaces) {
        if (nif.is_loopback()) {
            continue;
        }
        auto i = std::find_if(nif.addresses().begin(), nif.addresses().end(), std::mem_fn(&net::inet_address::is_ipv6));
        if (i == nif.addresses().end()) {
            continue;
        }

        std::ostringstream ss;
        ss << net::inet_address(i->as_ipv6_address()) << "%" << nif.name();
        auto text = ss.str();

        net::inet_address na(text);

        BOOST_REQUIRE_EQUAL(na.as_ipv6_address(), i->as_ipv6_address());
        // also verify that the inet_address itself matches
        BOOST_REQUIRE_EQUAL(na, *i);
        // and that inet_address _without_ scope matches.
        BOOST_REQUIRE_EQUAL(net::inet_address(na.as_ipv6_address()), *i);
        BOOST_REQUIRE_EQUAL(na.scope(), nif.index());
        // and that they are not ipv4 addresses
        BOOST_REQUIRE_THROW(i->as_ipv4_address(), std::invalid_argument);
        BOOST_REQUIRE_THROW(na.as_ipv4_address(), std::invalid_argument);

        niflog.info("Org: {}, Parsed: {}, Text: {}", *i, na, text);

    }

    return make_ready_future();
}

SEASTAR_TEST_CASE(is_standard_addresses_sanity) {
    BOOST_REQUIRE_EQUAL(net::inet_address("127.0.0.1").is_loopback(), true);
    BOOST_REQUIRE_EQUAL(net::inet_address("127.0.0.11").is_loopback(), true);
    BOOST_REQUIRE_EQUAL(net::inet_address(::in_addr{INADDR_ANY}).is_addr_any(), true);
    auto addr = net::inet_address("1.2.3.4");
    BOOST_REQUIRE_EQUAL(addr.is_loopback(), false);
    BOOST_REQUIRE_EQUAL(addr.is_addr_any(), false);

    BOOST_REQUIRE_EQUAL(net::inet_address("::1").is_loopback(), true);
    BOOST_REQUIRE_EQUAL(net::inet_address(::in6addr_any).is_addr_any(), true);
    auto addr6 = net::inet_address("acf1:f5e5:5a99:337f:ebe2:c57e:0e27:69c6");
    BOOST_REQUIRE_EQUAL(addr6.is_loopback(), false);
    BOOST_REQUIRE_EQUAL(addr6.is_addr_any(), false);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_inet_address_format) {
    const std::string tests[] = {
        // IPv4 addresses
        "127.0.0.1",
        "192.168.100.123",
        // IPv6 addresses
        // see also https://datatracker.ietf.org/doc/html/rfc5952
        // leading zeros are removed
        "2001:db8:85a3:8d3:1319:8a2e:370:7348",
        // consecutive all-zeros must be compressed
        "2001:db8::8a2e:370:7334",
        "::1",
        "100::",
    };

    for (auto expected : tests) {
        net::inet_address addr{expected};
        BOOST_CHECK_EQUAL(fmt::to_string(addr), expected);
    }

    // scoped addresses
    for (auto& nwif: seastar::engine().net().network_interfaces()) {
        const std::string_view address = "fe80::1ff:fe23:4567:890a";
        const sstring zone_id = fmt::to_string(nwif.index());
        for (auto zone : {zone_id, nwif.name(), nwif.display_name()}) {
            net::inet_address addr{fmt::format("{}%{}", address, zone)};
            // we always use the zone-id to represent the zone
            auto expected = fmt::format("{}%{}", address, zone_id);
            BOOST_CHECK_EQUAL(fmt::to_string(addr), expected);
        }
        // one of them would suffice
        break;
    }
    return make_ready_future();
}

SEASTAR_TEST_CASE(test_inet_address_parse_invalid) {
    const sstring tests[] = {
        // bad IPv4 addresses
        "127.0.0",
        "192.168.100,123",
        "192.168.1.2.3",
        // bad IPv6 addresses
        "fe80:2030:31:24",
        "fe80:2030:12345",
        "fe80:2030:12345%%",
        ":",
    };

    for (auto s : tests) {
        BOOST_CHECK_THROW(net::inet_address{s}, std::invalid_argument);
    }
    return make_ready_future();
}
