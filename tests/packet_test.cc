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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */


#define BOOST_TEST_MODULE core

#include <boost/test/included/unit_test.hpp>
#include "net/packet.hh"
#include <array>

using namespace net;

BOOST_AUTO_TEST_CASE(test_headers_are_contiguous) {
    using tcp_header = std::array<char, 20>;
    using ip_header = std::array<char, 20>;
    char data[1000] = {};
    fragment f{data, sizeof(data)};
    packet p(f);
    p.prepend_header<tcp_header>();
    p.prepend_header<ip_header>();
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 2);
}

BOOST_AUTO_TEST_CASE(test_headers_are_contiguous_even_with_small_fragment) {
    using tcp_header = std::array<char, 20>;
    using ip_header = std::array<char, 20>;
    char data[100] = {};
    fragment f{data, sizeof(data)};
    packet p(f);
    p.prepend_header<tcp_header>();
    p.prepend_header<ip_header>();
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 2);
}

BOOST_AUTO_TEST_CASE(test_headers_are_contiguous_even_with_many_fragments) {
    using tcp_header = std::array<char, 20>;
    using ip_header = std::array<char, 20>;
    char data[100] = {};
    fragment f{data, sizeof(data)};
    packet p(f);
    for (int i = 0; i < 7; ++i) {
        p.append(packet(f));
    }
    p.prepend_header<tcp_header>();
    p.prepend_header<ip_header>();
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 9);
}

