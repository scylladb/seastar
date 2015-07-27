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
 * Copyright 2015 Cloudius Systems
 */

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE core

#include <boost/test/included/unit_test.hpp>
#include "net/packet.hh"

static constexpr size_t internal_data_size = 128 - 16;
static constexpr size_t default_nr_frags = 4;
static constexpr size_t reserved_frags = 2;

// packet();
BOOST_AUTO_TEST_CASE(constructor1) {
    net::packet p;
    BOOST_REQUIRE_EQUAL(p.len(), 0);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 0);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
}

// packet(size_t nr_frags);
BOOST_AUTO_TEST_CASE(constructor2) {
    net::packet p(10);
    BOOST_REQUIRE_EQUAL(p.len(), 0);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 0);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), 10 + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
}

// packet(packet&& x) noexcept;
BOOST_AUTO_TEST_CASE(constructor3) {
    char buf[100];
    net::fragment f = { buf, 100 };
    net::packet p0(f);
    net::packet p(std::move(p0));
    BOOST_REQUIRE_EQUAL(p.len(), f.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 1);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[0].base != f.base); // copied
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
}

// packet(const char* data, size_t len);
BOOST_AUTO_TEST_CASE(constructor4) {
    char buf[100];
    net::packet p(buf, 100);
    BOOST_REQUIRE_EQUAL(p.len(), 100);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 1);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[0].base != buf); // copied
    BOOST_REQUIRE_EQUAL(frags[0].size, 100);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
}
// packet(fragment frag);
BOOST_AUTO_TEST_CASE(constructor5) {
    char buf[100];
    net::fragment f = { buf, 100 };
    net::packet p(f);
    BOOST_REQUIRE_EQUAL(p.len(), f.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 1);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[0].base != f.base); // copied
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
}

// packet(fragment frag, deleter del);
BOOST_AUTO_TEST_CASE(constructor6) {
    char buf[100];
    net::fragment f = { buf, 100 };
    deleter del;
    net::packet p(f, std::move(del));
    BOOST_REQUIRE_EQUAL(p.len(), f.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 1);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[0].base == f.base); // not copied
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
}

// packet(std::vector<fragment> frag, deleter del);
BOOST_AUTO_TEST_CASE(constructor7) {
    char buf[100];
    char buf2[200];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    std::vector<net::fragment> vec = { f, f2 };
    deleter del;
    net::packet p(vec, std::move(del));
    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 2);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[0].base == f.base); // not copied
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f2.base); // not copied
    BOOST_REQUIRE_EQUAL(frags[1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
}

// packet(packet&& x, fragment frag);
BOOST_AUTO_TEST_CASE(constructor8) {
    char buf[100];
    char buf2[200];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    deleter del;
    net::packet p0(f, std::move(del));
    net::packet p(std::move(p0), f2);
    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 2);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[0].base == f.base); // not copied
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base != f2.base); // copied
    BOOST_REQUIRE_EQUAL(frags[1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
}

// packet::packet(fragment frag, packet&& x)
BOOST_AUTO_TEST_CASE(constructor9) {
    char buf[100];
    char buf2[200];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    deleter del;
    net::packet p0(f, std::move(del));
    net::packet p(f2, std::move(p0));
    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 2);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 0);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), false);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[0].base != f2.base); // copied
    BOOST_REQUIRE_EQUAL(frags[0].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f.base); // not copied
    BOOST_REQUIRE_EQUAL(frags[1].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
}

// packet::packet(packet&& x, fragment frag, deleter d)
BOOST_AUTO_TEST_CASE(constructor10) {
    char buf[100];
    char buf2[200];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    deleter del, del2;
    net::packet p0(f, std::move(del));
    net::packet p(std::move(p0), f2, std::move(del2));
    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 2);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[0].base == f.base); // not copied
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f2.base); // not copied
    BOOST_REQUIRE_EQUAL(frags[1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_front1) {
    char buf[100];
    char buf2[200];
    char buf3[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    std::vector<net::fragment> vec = { f, f2, f3 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_front(50);
    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size + f3.size - 50);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 3);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), false);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-1].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 50);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, true);
    BOOST_REQUIRE(frags[0].base == f.base + 50);
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size - 50);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[2].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[2].size, f3.size);
    BOOST_REQUIRE_EQUAL(frags[2].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_front2) {
    char buf[100];
    char buf2[200];
    char buf3[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    std::vector<net::fragment> vec = { f, f2, f3 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_front(f.size);
    BOOST_REQUIRE_EQUAL(p.len(), f2.size + f3.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 2);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 2);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), false);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-1].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[-1].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[0].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[0].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[1].size, f3.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_front3) {
    char buf[300];
    char *buf2 = buf + 100;
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    std::vector<net::fragment> vec = { f, f2 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_front(300);
    BOOST_REQUIRE_EQUAL(p.len(), 0);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 0);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 3);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), false);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-3].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-3].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-3].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-2].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[-2].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[-2].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[-1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_front4) {
    char buf[300];
    char *buf2 = buf + 100;
    char buf3[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    std::vector<net::fragment> vec = { f, f2, f3 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_front(300);
    BOOST_REQUIRE_EQUAL(p.len(), f3.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 1);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 3);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), false);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-3].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-3].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-3].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-2].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[-2].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[-2].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[-1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[0].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[0].size, f3.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_front5) {
    char buf[300];
    char *buf2 = buf + 100;
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_front(300);
    BOOST_REQUIRE_EQUAL(p.len(), f3.size + f4.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 2);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 3);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), false);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-3].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-3].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-3].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-2].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[-2].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[-2].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[-1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[0].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[0].size, f3.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[1].size, f4.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_front6) {
    char buf[300];
    char *buf2 = buf + 100;
    char spacer[8] __attribute__((unused));
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_front(350);

    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size + f3.size + f4.size - 350);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 2);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 3);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), false);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-3].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[-3].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[-3].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-2].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[-2].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[-2].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 50);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, true);
    BOOST_REQUIRE(frags[0].base == f3.base + 50);
    BOOST_REQUIRE_EQUAL(frags[0].size, f3.size - 50);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[1].size, f4.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_front7) {
    char buf[300];
    char *buf2 = buf + 100;
    char spacer[8] __attribute__((unused));
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_front(350);
    p.trim_front(50);

    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size + f3.size + f4.size - 400);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 2);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 3);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), false);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-3].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[-3].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[-3].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-2].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[-2].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[-2].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 100);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, true);
    BOOST_REQUIRE(frags[0].base == f3.base + 100);
    BOOST_REQUIRE_EQUAL(frags[0].size, f3.size - 100);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[1].size, f4.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_front8) {
    char buf[300];
    char *buf2 = buf + 100;
    char spacer[8] __attribute__((unused));
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_front(350);
    p.trim_front(50);
    p.trim_front(200);

    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size + f3.size + f4.size - 600);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 1);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 3);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), false);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-3].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[-3].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[-3].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-2].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[-2].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[-2].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[-1].size, f3.size);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[0].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[0].size, f4.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_front9) {
    char buf[300];
    char *buf2 = buf + 100;
    char spacer[8] __attribute__((unused));
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_front(350);
    p.trim_front(50);
    p.trim_front(200);
    p.trim_front(30);

    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size + f3.size + f4.size - 630);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 1);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 4);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), false);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-4].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[-4].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[-4].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-3].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[-3].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[-3].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-2].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[-2].size, f3.size);
    BOOST_REQUIRE_EQUAL(frags[-2].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 30);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, true);
    BOOST_REQUIRE(frags[0].base == f4.base + 30);
    BOOST_REQUIRE_EQUAL(frags[0].size, f4.size - 30);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_front10) {
    char buf[300];
    char *buf2 = buf + 100;
    char spacer[8] __attribute__((unused));
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_front(350);
    p.trim_front(50);
    p.trim_front(200);
    p.trim_front(30);
    p.trim_front(70);

    BOOST_REQUIRE_EQUAL(p.len(), 0);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 0);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 4);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), false);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-4].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[-4].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[-4].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-3].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[-3].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[-3].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-2].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[-2].size, f3.size);
    BOOST_REQUIRE_EQUAL(frags[-2].can_merge_with_next, false);
    BOOST_REQUIRE(frags[-1].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[-1].size, f4.size);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(untrim_front) {
    char buf[300];
    char *buf2 = buf + 100;
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_front(300);
    p.untrim_front();
    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size + f3.size + f4.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 4);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[0].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[2].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[2].size, f3.size);
    BOOST_REQUIRE_EQUAL(frags[2].can_merge_with_next, false);
    BOOST_REQUIRE(frags[3].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[3].size, f4.size);
    BOOST_REQUIRE_EQUAL(frags[3].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_back1) {
    char buf[300];
    char *buf2 = buf + 100;
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_back(100);
    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size + f3.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 3);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 1);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[0].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[2].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[2].size, f3.size);
    BOOST_REQUIRE_EQUAL(frags[2].can_merge_with_next, false);
    BOOST_REQUIRE(frags[3].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[3].size, f4.size);
    BOOST_REQUIRE_EQUAL(frags[3].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_back2) {
    char buf[300];
    char *buf2 = buf + 100;
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_back(40);
    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size + f3.size + f4.size - 40);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 4);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 1);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[0].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[2].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[2].size, f3.size);
    BOOST_REQUIRE_EQUAL(frags[2].can_merge_with_next, false);
    BOOST_REQUIRE(frags[3].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[3].size, f4.size - 40);
    BOOST_REQUIRE_EQUAL(frags[3].can_merge_with_next, true);
    BOOST_REQUIRE(frags[4].base == f4.base + (f4.size - 40));
    BOOST_REQUIRE_EQUAL(frags[4].size, 40);
    BOOST_REQUIRE_EQUAL(frags[4].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_back3) {
    char buf[300];
    char *buf2 = buf + 100;
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_back(150);
    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size + f3.size + f4.size - 150);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 3);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 2);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[0].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[2].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[2].size, f3.size - 50);
    BOOST_REQUIRE_EQUAL(frags[2].can_merge_with_next, true);
    BOOST_REQUIRE(frags[3].base == f3.base + (f3.size - 50));
    BOOST_REQUIRE_EQUAL(frags[3].size, 50);
    BOOST_REQUIRE_EQUAL(frags[3].can_merge_with_next, false);
    BOOST_REQUIRE(frags[4].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[4].size, f4.size);
    BOOST_REQUIRE_EQUAL(frags[4].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_back4) {
    char buf[300];
    char *buf2 = buf + 100;
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_back(150);
    p.trim_back(50);
    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size + f3.size + f4.size - 200);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 3);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 2);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[0].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[2].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[2].size, f3.size - 100);
    BOOST_REQUIRE_EQUAL(frags[2].can_merge_with_next, true);
    BOOST_REQUIRE(frags[2].base == f3.base);
    BOOST_REQUIRE(frags[3].base == f3.base + (f3.size - 100));
    BOOST_REQUIRE_EQUAL(frags[3].size, 100);
    BOOST_REQUIRE_EQUAL(frags[3].can_merge_with_next, false);
    BOOST_REQUIRE(frags[4].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[4].size, f4.size);
    BOOST_REQUIRE_EQUAL(frags[4].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(trim_back5) {
    char buf[300];
    char *buf2 = buf + 100;
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_back(150);
    p.trim_back(50);
    p.trim_back(200);
    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 2);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 2);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE(frags[0].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE(frags[1].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[1].size, f2.size);
    BOOST_REQUIRE(frags[2].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[2].size, f3.size);
    BOOST_REQUIRE(frags[3].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[3].size, f4.size);
}

BOOST_AUTO_TEST_CASE(untrim_back1) {
    char buf[300];
    char *buf2 = buf + 100;
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_back(100);
    p.untrim_back();
    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size + f3.size + f4.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 4);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[0].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[2].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[2].size, f3.size);
    BOOST_REQUIRE_EQUAL(frags[2].can_merge_with_next, false);
    BOOST_REQUIRE(frags[3].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[3].size, f4.size);
    BOOST_REQUIRE_EQUAL(frags[3].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(untrim_back2) {
    char buf[300];
    char *buf2 = buf + 100;
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_back(40);
    p.untrim_back();
    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size + f3.size + f4.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 5);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[0].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[2].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[2].size, f3.size);
    BOOST_REQUIRE_EQUAL(frags[2].can_merge_with_next, false);
    BOOST_REQUIRE(frags[3].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[3].size, f4.size - 40);
    BOOST_REQUIRE_EQUAL(frags[3].can_merge_with_next, true);
    BOOST_REQUIRE(frags[4].base == f4.base + (f4.size - 40));
    BOOST_REQUIRE_EQUAL(frags[4].size, 40);
    BOOST_REQUIRE_EQUAL(frags[4].can_merge_with_next, false);
}

BOOST_AUTO_TEST_CASE(untrim_back3) {
    char buf[300];
    char *buf2 = buf + 100;
    char buf3[300];
    char buf4[300];
    net::fragment f = { buf, 100 };
    net::fragment f2 = { buf2, 200 };
    net::fragment f3 = { buf3, 300 };
    net::fragment f4 = { buf4, 100 };
    std::vector<net::fragment> vec = { f, f2, f3, f4 };
    deleter del;
    net::packet p(vec, std::move(del));
    p.trim_back(150);
    p.untrim_back();
    BOOST_REQUIRE_EQUAL(p.len(), f.size + f2.size + f3.size + f4.size);
    BOOST_REQUIRE_EQUAL(p.nr_frags(), 5);
    BOOST_REQUIRE_EQUAL(p.allocated_frags(), default_nr_frags + reserved_frags);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_front(), 1);
    BOOST_REQUIRE_EQUAL(p.nr_trimmed_back(), 0);
    BOOST_REQUIRE_EQUAL(p.reserved_front_usable(), true);
    net::fragment *frags = p.fragment_array();
    BOOST_REQUIRE(frags[-1].base == nullptr);
    BOOST_REQUIRE_EQUAL(frags[-1].size, 0);
    BOOST_REQUIRE_EQUAL(frags[-1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[0].base == f.base);
    BOOST_REQUIRE_EQUAL(frags[0].size, f.size);
    BOOST_REQUIRE_EQUAL(frags[0].can_merge_with_next, false);
    BOOST_REQUIRE(frags[1].base == f2.base);
    BOOST_REQUIRE_EQUAL(frags[1].size, f2.size);
    BOOST_REQUIRE_EQUAL(frags[1].can_merge_with_next, false);
    BOOST_REQUIRE(frags[2].base == f3.base);
    BOOST_REQUIRE_EQUAL(frags[2].size, f3.size - 50);
    BOOST_REQUIRE_EQUAL(frags[2].can_merge_with_next, true);
    BOOST_REQUIRE(frags[3].base == f3.base + (f3.size - 50));
    BOOST_REQUIRE_EQUAL(frags[3].size, 50);
    BOOST_REQUIRE_EQUAL(frags[3].can_merge_with_next, false);
    BOOST_REQUIRE(frags[4].base == f4.base);
    BOOST_REQUIRE_EQUAL(frags[4].size, f4.size);
    BOOST_REQUIRE_EQUAL(frags[4].can_merge_with_next, false);
}
