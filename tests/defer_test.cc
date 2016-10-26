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
 * Copyright 2016 ScyllaDB
 */

#define BOOST_TEST_MODULE core

#include <boost/test/included/unit_test.hpp>
#include "util/defer.hh"

BOOST_AUTO_TEST_CASE(test_defer_does_not_run_when_canceled) {
    bool ran = false;
    {
        auto d = defer([&] {
            ran = true;
        });
        d.cancel();
    }
    BOOST_REQUIRE(!ran);
}

BOOST_AUTO_TEST_CASE(test_defer_runs) {
    bool ran = false;
    {
        auto d = defer([&] {
            ran = true;
        });
    }
    BOOST_REQUIRE(ran);
}

BOOST_AUTO_TEST_CASE(test_defer_runs_once_when_moved) {
    int ran = 0;
    {
        auto d = defer([&] {
            ++ran;
        });
        {
            auto d2 = std::move(d);
        }
        BOOST_REQUIRE_EQUAL(1, ran);
    }
    BOOST_REQUIRE_EQUAL(1, ran);
}

BOOST_AUTO_TEST_CASE(test_defer_does_not_run_when_moved_after_cancelled) {
    int ran = 0;
    {
        auto d = defer([&] {
            ++ran;
        });
        d.cancel();
        {
            auto d2 = std::move(d);
        }
    }
    BOOST_REQUIRE_EQUAL(0, ran);
}
