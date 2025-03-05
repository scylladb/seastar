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

#include <boost/test/unit_test.hpp>
#include <stdlib.h>
#include <chrono>
#include <deque>
#include <random>
#include <ranges>
#if __has_include(<version>)
#include <version>
#endif

#include <seastar/core/circular_buffer.hh>
#include <seastar/util/assert.hh>

using namespace seastar;

static_assert(std::ranges::range<circular_buffer<int>>);

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
    SEASTAR_ASSERT(*ptr_to_3 == 3);
    SEASTAR_ASSERT(*iterator_to_3 == 3);

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

BOOST_AUTO_TEST_CASE(test_underflow_index_iterator_comparison) {
    circular_buffer<int> buf;

    const auto seed = std::random_device()();
    std::cout << "seed=" << seed << std::endl;
    auto rnd_engine = std::mt19937(seed);
    std::uniform_int_distribution<unsigned> count_dist(0, 20);
    std::uniform_int_distribution<unsigned> bool_dist(false, true);

    auto push_back = [&buf] (unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            buf.push_back(i);
        }
    };
    auto push_front = [&buf] (unsigned n) {
        for (unsigned i = 0; i < n; ++i) {
            buf.push_front(i);
        }
    };

    for (unsigned i = 0; i < 16; ++i) {
        const auto push_back_count = count_dist(rnd_engine);
        const auto push_front_count = count_dist(rnd_engine);
        std::cout << "round[" << i << "]: " << buf.size() << " front: " << push_front_count << " back: " << push_back_count << std::endl;
        if (bool_dist(rnd_engine)) {
            push_back(push_back_count);
            push_front(std::max(20 - push_back_count, push_front_count));
        } else {
            push_front(push_front_count);
            push_back(std::max(20 - push_front_count, push_back_count));
        }

        if (buf.empty()) {
            continue;
        }

        for (auto it1 = buf.begin(); it1 != buf.end(); ++it1) {
            bool bypass = false;
            for (auto it2 = buf.end(); it2 != buf.begin(); --it2) {
                auto itl = it1;
                auto ith = it2;
                if (bypass) {
                    std::swap(itl, ith);
                }
                if (itl == ith) {
                    bypass = true;
                } else {
                    BOOST_REQUIRE(itl < ith);
                    BOOST_REQUIRE(ith > itl);
                    BOOST_REQUIRE(!(ith < itl));
                    BOOST_REQUIRE(!(itl > ith));
                }

                BOOST_REQUIRE(itl <= ith);
                BOOST_REQUIRE(itl <= itl);
                BOOST_REQUIRE(ith <= ith);
                BOOST_REQUIRE(ith >= itl);
                BOOST_REQUIRE(itl >= itl);
                BOOST_REQUIRE(ith >= ith);
            }
        }

        const auto erase_count = count_dist(rnd_engine);
        const auto offset = count_dist(rnd_engine);
        std::cout << "round[" << i << "]: " << erase_count << " @ " << offset << std::endl;
        buf.erase(buf.begin() + offset, buf.begin() + std::min(size_t(offset + erase_count), buf.size()));
    }
}
