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
 * Copyright (C) 2017 ScyllaDB Ltd.
 */


#define BOOST_TEST_MODULE core

#include <boost/test/included/unit_test.hpp>
#include <stdlib.h>
#include <chrono>
#include <deque>
#include <seastar/core/circular_buffer.hh>

using namespace seastar;

BOOST_AUTO_TEST_CASE(test_erasing) {
    circular_buffer<int> buf;

    buf.push_back(3);
    buf.erase(buf.begin(), buf.end());

    BOOST_REQUIRE(buf.size() == 0);
    BOOST_REQUIRE(buf.empty());

    buf.push_back(1);
    buf.push_back(2);
    buf.push_back(3);
    buf.push_back(4);
    buf.push_back(5);

    buf.erase(std::remove_if(buf.begin(), buf.end(), [] (int v) { return (v & 1) == 0; }), buf.end());

    BOOST_REQUIRE(buf.size() == 3);
    BOOST_REQUIRE(!buf.empty());
    {
        auto i = buf.begin();
        BOOST_REQUIRE_EQUAL(*i++, 1);
        BOOST_REQUIRE_EQUAL(*i++, 3);
        BOOST_REQUIRE_EQUAL(*i++, 5);
        BOOST_REQUIRE(i == buf.end());
    }
}

BOOST_AUTO_TEST_CASE(test_erasing_at_beginning_or_end_does_not_invalidate_iterators) {
    // This guarantee comes from std::deque, which circular_buffer is supposed to mimic.

    circular_buffer<int> buf;

    buf.push_back(1);
    buf.push_back(2);
    buf.push_back(3);
    buf.push_back(4);
    buf.push_back(5);

    int* ptr_to_3 = &buf[2];
    auto iterator_to_3 = buf.begin() + 2;
    assert(*ptr_to_3 == 3);
    assert(*iterator_to_3 == 3);

    buf.erase(buf.begin(), buf.begin() + 2);

    BOOST_REQUIRE(*ptr_to_3 == 3);
    BOOST_REQUIRE(*iterator_to_3 == 3);

    buf.erase(buf.begin() + 1, buf.end());

    BOOST_REQUIRE(*ptr_to_3 == 3);
    BOOST_REQUIRE(*iterator_to_3 == 3);

    BOOST_REQUIRE(buf.size() == 1);
}

BOOST_AUTO_TEST_CASE(test_erasing_in_the_middle) {
    circular_buffer<int> buf;

    for (int i = 0; i < 10; ++i) {
        buf.push_back(i);
    }

    auto i = buf.erase(buf.begin() + 3, buf.begin() + 6);
    BOOST_REQUIRE_EQUAL(*i, 6);

    i = buf.begin();
    BOOST_REQUIRE_EQUAL(*i++, 0);
    BOOST_REQUIRE_EQUAL(*i++, 1);
    BOOST_REQUIRE_EQUAL(*i++, 2);
    BOOST_REQUIRE_EQUAL(*i++, 6);
    BOOST_REQUIRE_EQUAL(*i++, 7);
    BOOST_REQUIRE_EQUAL(*i++, 8);
    BOOST_REQUIRE_EQUAL(*i++, 9);
    BOOST_REQUIRE(i == buf.end());
}
