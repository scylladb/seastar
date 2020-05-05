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
#include <deque>
#include <random>
#include <seastar/core/circular_buffer_fixed_capacity.hh>

#include <boost/range/algorithm/sort.hpp>
#include <boost/range/algorithm/equal.hpp>
#include <boost/range/algorithm/reverse.hpp>

using namespace seastar;

using cb16_t = circular_buffer_fixed_capacity<int, 16>;

struct int_with_stats {
    int val;
    unsigned *num_deleted;
    unsigned *num_moved;
    operator int() const { return val; }
    int_with_stats(int val, unsigned* num_deleted, unsigned* num_moved)
        : val(val), num_deleted(num_deleted), num_moved(num_moved) {}

    ~int_with_stats() { ++(*num_deleted); }
    int_with_stats(const int_with_stats&) = delete;
    int_with_stats(int_with_stats&& o) noexcept : val(o.val), num_deleted(o.num_deleted), num_moved(o.num_moved) {
        ++(*num_moved);
    }
    int_with_stats& operator=(int_with_stats&& o) noexcept {
        this->~int_with_stats();
        new (this) int_with_stats(std::move(o));
        return *this;
    }
};

BOOST_AUTO_TEST_CASE(test_edge_cases) {
    unsigned num_deleted = 0;
    unsigned num_moved = 0;
    auto get_val = [&num_deleted, &num_moved] (int val) {
        return int_with_stats{val, &num_deleted, &num_moved};
    };

    {
        circular_buffer_fixed_capacity<int_with_stats, 16> cb;
        BOOST_REQUIRE(cb.begin() == cb.end());
        cb.push_front(get_val(3));  // underflows indexes
        BOOST_REQUIRE_EQUAL(cb[0], 3);
        BOOST_REQUIRE(cb.begin() < cb.end());
        cb.push_back(get_val(4));
        BOOST_REQUIRE_EQUAL(cb.size(), 2u);
        BOOST_REQUIRE_EQUAL(cb[0], 3);
        BOOST_REQUIRE_EQUAL(cb[1], 4);
        cb.pop_back();
        BOOST_REQUIRE_EQUAL(cb.back(), 3);
        cb.push_front(get_val(1));
        cb.pop_back();
        BOOST_REQUIRE_EQUAL(cb.back(), 1);

        BOOST_REQUIRE_EQUAL(num_deleted, 5);
        BOOST_REQUIRE_EQUAL(num_moved, 3);

        cb.push_front(get_val(0));
        cb.push_back(get_val(2));
        BOOST_REQUIRE_EQUAL(cb.size(), 3);
        BOOST_REQUIRE_EQUAL(cb[0], 0);
        BOOST_REQUIRE_EQUAL(cb[1], 1);
        BOOST_REQUIRE_EQUAL(cb[2], 2);
        BOOST_REQUIRE_EQUAL(num_deleted, 7);
        BOOST_REQUIRE_EQUAL(num_moved, 5);

        circular_buffer_fixed_capacity<int_with_stats, 16> cb2 = std::move(cb);
        BOOST_REQUIRE_EQUAL(cb2.size(), 3);
        BOOST_REQUIRE_EQUAL(cb2[0], 0);
        BOOST_REQUIRE_EQUAL(cb2[1], 1);
        BOOST_REQUIRE_EQUAL(cb2[2], 2);
        BOOST_REQUIRE_EQUAL(num_deleted, 7);
        BOOST_REQUIRE_EQUAL(num_moved, 8);
    }
    BOOST_REQUIRE_EQUAL(num_deleted, 13);
    BOOST_REQUIRE_EQUAL(num_moved, 8);
}

using deque = std::deque<int>;

BOOST_AUTO_TEST_CASE(test_random_walk) {
    auto rand = std::default_random_engine();
    auto op_gen = std::uniform_int_distribution<unsigned>(0, 6);
    deque d;
    cb16_t c;
    for (auto i = 0; i != 1000000; ++i) {
        auto op = op_gen(rand);
        switch (op) {
        case 0:
            if (d.size() < 16) {
                auto n = rand();
                c.push_back(n);
                d.push_back(n);
            }
            break;
        case 1:
            if (d.size() < 16) {
                auto n = rand();
                c.push_front(n);
                d.push_front(n);
            }
            break;
        case 2:
            if (!d.empty()) {
                auto n = d.back();
                auto m = c.back();
                BOOST_REQUIRE_EQUAL(n, m);
                c.pop_back();
                d.pop_back();
            }
            break;
        case 3:
            if (!d.empty()) {
                auto n = d.front();
                auto m = c.front();
                BOOST_REQUIRE_EQUAL(n, m);
                c.pop_front();
                d.pop_front();
            }
            break;
        case 4:
            boost::sort(c);
            boost::sort(d);
            break;
        case 5:
            if (!d.empty()) {
                auto u = std::uniform_int_distribution<size_t>(0, d.size() - 1);
                auto idx = u(rand);
                auto m = c[idx];
                auto n = c[idx];
                BOOST_REQUIRE_EQUAL(m, n);
            }
            break;
        case 6:
            c.clear();
            d.clear();
            break;
        case 7:
            boost::reverse(c);
            boost::reverse(d);
        default:
            abort();
        }
        BOOST_REQUIRE_EQUAL(c.size(), d.size());
        BOOST_REQUIRE(boost::equal(c, d));
    }
}
