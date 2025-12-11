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
 * Copyright (C) 2025 ScyllaDB
 */


#define BOOST_TEST_MODULE core

#include <fmt/core.h>
#include <boost/test/unit_test.hpp>
#include <seastar/util/split-list.hh>
#include <random>

// A fuzzy test that populates split-list randomly from both ends and then
// checks that forward scanning reveals consistent results
BOOST_AUTO_TEST_CASE(test_split_list_grow_and_shrink) {
    const auto seed = std::random_device()();
    auto rnd_engine = std::mt19937(seed);
    auto dist = std::uniform_int_distribution<unsigned>(0, std::numeric_limits<char>::max());

    struct entry {
        int value;
        entry* next;
        entry(int v) noexcept : value(v), next(nullptr) {}
    };

    seastar::internal::intrusive_split_list<entry, 5, &entry::next> sl;

    int first = 0;
    int last = 0;
    for (unsigned i = 0; i < 1234; i++) {
        auto op = dist(rnd_engine) % 5;
        if (op == 0) { // push back
            fmt::print("+> {}\n", last + 1);
            entry* e = new entry(++last);
            sl.push_back(e);
        } else if (op == 1) { // push front
            fmt::print("<+ {}\n", first);
            entry* e = new entry(first--);
            sl.push_front(e);
        } else if (op == 2) { // scan
            unsigned scanned = 0;
            fmt::print("= {} ... {}\n", first, last);
            for (auto it = sl.begin(); it != sl.end(); ++it) {
                fmt::print("  {}\n", it->value);
                BOOST_REQUIRE_EQUAL(it->value, first + 1 + scanned);
                scanned++;
            }
            BOOST_REQUIRE_EQUAL(scanned, last - first);
        } else { // pop (from front)
            if (sl.empty()) {
                fmt::print("x\n");
                BOOST_REQUIRE_EQUAL(first, last);
                continue;
            }

            fmt::print("<- {}\n", first + 1);
            entry* e = sl.pop_front();
            BOOST_REQUIRE_NE(first, last);
            BOOST_REQUIRE_EQUAL(e->value, ++first);
            delete e;
        }
    }

    while (first != last) {
        BOOST_REQUIRE(!sl.empty());
        entry* e = sl.pop_front();
        BOOST_REQUIRE_EQUAL(e->value, ++first);
        delete e;
    }
    BOOST_REQUIRE(sl.empty());
}
