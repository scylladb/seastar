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
 * Copyright (C) 2016 ScyllaDB
 */

#include "core/thread.hh"
#include "test-utils.hh"
#include "core/future-util.hh"
#include "core/expiring_fifo.hh"
#include <boost/range/irange.hpp>

using namespace seastar;
using namespace std::chrono_literals;

SEASTAR_TEST_CASE(test_no_expiry_operations) {
    expiring_fifo<int> fifo;

    BOOST_REQUIRE(fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE(!bool(fifo));

    fifo.push_back(1);

    BOOST_REQUIRE(!fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
    BOOST_REQUIRE(bool(fifo));
    BOOST_REQUIRE_EQUAL(fifo.front(), 1);

    fifo.push_back(2);
    fifo.push_back(3);

    BOOST_REQUIRE(!fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 3u);
    BOOST_REQUIRE(bool(fifo));
    BOOST_REQUIRE_EQUAL(fifo.front(), 1);

    fifo.pop_front();

    BOOST_REQUIRE(!fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 2u);
    BOOST_REQUIRE(bool(fifo));
    BOOST_REQUIRE_EQUAL(fifo.front(), 2);

    fifo.pop_front();

    BOOST_REQUIRE(!fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
    BOOST_REQUIRE(bool(fifo));
    BOOST_REQUIRE_EQUAL(fifo.front(), 3);

    fifo.pop_front();

    BOOST_REQUIRE(fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE(!bool(fifo));

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_expiry_operations) {
    return seastar::async([] {
        std::vector<int> expired;
        struct my_expiry {
            std::vector<int>& e;
            void operator()(int& v) { e.push_back(v); }
        };

        expiring_fifo<int, my_expiry, manual_clock> fifo(my_expiry{expired});

        fifo.push_back(1, manual_clock::now() + 1s);

        BOOST_REQUIRE(!fifo.empty());
        BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
        BOOST_REQUIRE(bool(fifo));
        BOOST_REQUIRE_EQUAL(fifo.front(), 1);

        manual_clock::advance(1s);
        later().get();

        BOOST_REQUIRE(fifo.empty());
        BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
        BOOST_REQUIRE(!bool(fifo));
        BOOST_REQUIRE_EQUAL(expired.size(), 1u);
        BOOST_REQUIRE_EQUAL(expired[0], 1);

        expired.clear();

        fifo.push_back(1);
        fifo.push_back(2, manual_clock::now() + 1s);
        fifo.push_back(3);

        manual_clock::advance(1s);
        later().get();

        BOOST_REQUIRE(!fifo.empty());
        BOOST_REQUIRE_EQUAL(fifo.size(), 2u);
        BOOST_REQUIRE(bool(fifo));
        BOOST_REQUIRE_EQUAL(expired.size(), 1u);
        BOOST_REQUIRE_EQUAL(expired[0], 2);
        BOOST_REQUIRE_EQUAL(fifo.front(), 1);
        fifo.pop_front();
        BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
        BOOST_REQUIRE_EQUAL(fifo.front(), 3);
        fifo.pop_front();
        BOOST_REQUIRE_EQUAL(fifo.size(), 0u);

        expired.clear();

        fifo.push_back(1, manual_clock::now() + 1s);
        fifo.push_back(2, manual_clock::now() + 1s);
        fifo.push_back(3);
        fifo.push_back(4, manual_clock::now() + 2s);

        manual_clock::advance(1s);
        later().get();

        BOOST_REQUIRE(!fifo.empty());
        BOOST_REQUIRE_EQUAL(fifo.size(), 2u);
        BOOST_REQUIRE(bool(fifo));
        BOOST_REQUIRE_EQUAL(expired.size(), 2u);
        std::sort(expired.begin(), expired.end());
        BOOST_REQUIRE_EQUAL(expired[0], 1);
        BOOST_REQUIRE_EQUAL(expired[1], 2);
        BOOST_REQUIRE_EQUAL(fifo.front(), 3);
        fifo.pop_front();
        BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
        BOOST_REQUIRE_EQUAL(fifo.front(), 4);
        fifo.pop_front();
        BOOST_REQUIRE_EQUAL(fifo.size(), 0u);

        expired.clear();

        fifo.push_back(1);
        fifo.push_back(2, manual_clock::now() + 1s);
        fifo.push_back(3, manual_clock::now() + 1s);
        fifo.push_back(4, manual_clock::now() + 1s);

        manual_clock::advance(1s);
        later().get();

        BOOST_REQUIRE(!fifo.empty());
        BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
        BOOST_REQUIRE(bool(fifo));
        BOOST_REQUIRE_EQUAL(expired.size(), 3u);
        std::sort(expired.begin(), expired.end());
        BOOST_REQUIRE_EQUAL(expired[0], 2);
        BOOST_REQUIRE_EQUAL(expired[1], 3);
        BOOST_REQUIRE_EQUAL(expired[2], 4);
        BOOST_REQUIRE_EQUAL(fifo.front(), 1);
        fifo.pop_front();
        BOOST_REQUIRE_EQUAL(fifo.size(), 0u);

        expired.clear();

        fifo.push_back(1);
        fifo.push_back(2, manual_clock::now() + 1s);
        fifo.push_back(3, manual_clock::now() + 1s);
        fifo.push_back(4, manual_clock::now() + 1s);
        fifo.push_back(5);

        manual_clock::advance(1s);
        later().get();

        BOOST_REQUIRE_EQUAL(fifo.size(), 2u);
        BOOST_REQUIRE_EQUAL(fifo.front(), 1);
        fifo.pop_front();
        BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
        BOOST_REQUIRE_EQUAL(fifo.front(), 5);
        fifo.pop_front();
        BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    });
}
