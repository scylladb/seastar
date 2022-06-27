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
 * Copyright (C) 2022 ScyllaDB
 */

#include <seastar/core/thread.hh>
#include <seastar/core/manual_clock.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/util/shared_token_bucket.hh>

using namespace seastar;
using namespace std::chrono_literals;

SEASTAR_TEST_CASE(test_basic_non_capped_loop) {
    internal::shared_token_bucket<uint64_t, std::ratio<1>, internal::capped_release::no, manual_clock> tb(1, 1, 0, false);

    // Grab one token and make sure it's only available in 1s
    auto th = tb.grab(1);
    BOOST_REQUIRE(tb.deficiency(th) > 0);
    manual_clock::advance(1s);
    tb.replenish(manual_clock::now());
    BOOST_REQUIRE(tb.deficiency(th) == 0);

    // Grab one more token and check the same
    th = tb.grab(1);
    BOOST_REQUIRE(tb.deficiency(th) > 0);
    manual_clock::advance(1s);
    tb.replenish(manual_clock::now());
    BOOST_REQUIRE(tb.deficiency(th) == 0);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_basic_capped_loop) {
    internal::shared_token_bucket<uint64_t, std::ratio<1>, internal::capped_release::yes, manual_clock> tb(1, 1, 0, false);

    // Grab on token and make sure it's only available in 1s
    auto th = tb.grab(1);
    BOOST_REQUIRE(tb.deficiency(th) > 0);
    manual_clock::advance(1s);
    tb.replenish(manual_clock::now());
    BOOST_REQUIRE(tb.deficiency(th) == 0);

    // The 2nd time this trick only works after the 1st token is explicitly released
    th = tb.grab(1);
    BOOST_REQUIRE(tb.deficiency(th) > 0);
    manual_clock::advance(1s);
    tb.replenish(manual_clock::now());
    BOOST_REQUIRE(tb.deficiency(th) > 0);
    manual_clock::advance(1s);
    tb.release(1);
    tb.replenish(manual_clock::now());
    BOOST_REQUIRE(tb.deficiency(th) == 0);

    return make_ready_future<>();
}
