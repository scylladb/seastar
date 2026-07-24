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

template <typename T = uint64_t, typename Period = std::ratio<1>>
using satb = internal::shard_aware_token_bucket<T, Period, manual_clock>;

SEASTAR_TEST_CASE(test_satb_single_class) {
    // rate=10 tokens/s, burst=10, threshold=1; one class owns all shares
    satb<> tb(10, 10, 1, false);
    satb<>::local_publisher pub(tb);
    satb<>::consumer cs(pub, 100);
    satb<>::tokens pouch(tb.limit());

    cs.activate();
    pub.flush();
    pouch.refill(cs.accrued());
    BOOST_REQUIRE_EQUAL(pouch.available(), 0u);

    manual_clock::advance(1s);
    tb.replenish(manual_clock::now());

    // gain = 10 * 100/100 = 10, capped at burst * 100/100 = 10
    pouch.refill(cs.accrued());
    BOOST_REQUIRE_EQUAL(pouch.available(), 10u);

    BOOST_REQUIRE(pouch.try_consume(7));
    BOOST_REQUIRE_EQUAL(pouch.available(), 3u);
    BOOST_REQUIRE(!pouch.try_consume(4));

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_satb_equal_shares) {
    // Two classes with equal shares should each receive half the tokens
    satb<> tb(10, 10, 1, false);
    satb<>::local_publisher pub(tb);
    satb<>::consumer cs1(pub, 100);
    satb<>::consumer cs2(pub, 100);
    satb<>::tokens p1(tb.limit());
    satb<>::tokens p2(tb.limit());

    cs1.activate();
    cs2.activate();
    // Flush both contributions before either accrues, so total_shares = 200.
    pub.flush();
    manual_clock::advance(1s);
    tb.replenish(manual_clock::now());

    // Each gets 10 * 100/200 = 5, burst cap = 10 * 100/200 = 5
    p1.refill(cs1.accrued());
    p2.refill(cs2.accrued());
    BOOST_REQUIRE_EQUAL(p1.available(), 5u);
    BOOST_REQUIRE_EQUAL(p2.available(), 5u);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_satb_unequal_shares) {
    // Class A has 1/3 of shares, class B has 2/3
    satb<> tb(9, 9, 1, false);
    satb<>::local_publisher pub(tb);
    satb<>::consumer cs_a(pub, 100);
    satb<>::consumer cs_b(pub, 200);
    satb<>::tokens p_a(tb.limit());
    satb<>::tokens p_b(tb.limit());

    cs_a.activate();
    cs_b.activate();
    pub.flush();
    manual_clock::advance(1s);
    tb.replenish(manual_clock::now());

    // cs_a: 9 * 100/300 = 3; cs_b: 9 * 200/300 = 6
    p_a.refill(cs_a.accrued());
    p_b.refill(cs_b.accrued());
    BOOST_REQUIRE_EQUAL(p_a.available(), 3u);
    BOOST_REQUIRE_EQUAL(p_b.available(), 6u);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_satb_burst_cap) {
    // Class doesn't poll for many seconds -- should be capped at burst limit on wakeup
    satb<> tb(10, 10, 1, false);
    satb<>::local_publisher pub(tb);
    satb<>::consumer cs(pub, 100);
    satb<>::tokens pouch(tb.limit());

    cs.activate();
    pub.flush();
    // 10 seconds without pouch.refill() -- far more tokens than burst
    for (int i = 0; i < 10; i++) {
        manual_clock::advance(1s);
        tb.replenish(manual_clock::now());
    }

    // burst cap = 10 * 100/100 = 10; not 100 (10 ticks * 10 rate)
    pouch.refill(cs.accrued());
    BOOST_REQUIRE_EQUAL(pouch.available(), 10u);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_satb_stuck_shard) {
    // Shard B polls every second and consumes its tokens.
    // Shard A is stuck (doesn't poll).
    // A should get exactly its burst cap when it finally wakes up;
    // B should not be affected by A's inactivity.
    satb<> tb(10, 10, 1, false);
    satb<>::local_publisher pub(tb);
    satb<>::consumer cs_a(pub, 100);  // shard A -- stuck
    satb<>::consumer cs_b(pub, 100);  // shard B -- active
    satb<>::tokens p_a(tb.limit());
    satb<>::tokens p_b(tb.limit());

    cs_a.activate();
    cs_b.activate();
    pub.flush();
    for (int i = 0; i < 5; i++) {
        manual_clock::advance(1s);
        tb.replenish(manual_clock::now());
        // B accumulates and drains; A does nothing
        p_b.refill(cs_b.accrued());
        while (p_b.try_consume(1)) {}
    }

    // B is at 0 after draining
    BOOST_REQUIRE_EQUAL(p_b.available(), 0u);

    // A wakes up: sees large delta but is capped at the full burst limit (10),
    // not at the proportional share.  Fairness comes from the gain formula,
    // not from capping what an idle consumer may accumulate.
    p_a.refill(cs_a.accrued());
    BOOST_REQUIRE_EQUAL(p_a.available(), 10u);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_satb_total_shares_tracking) {
    satb<> tb(10, 10, 1, false);
    satb<>::local_publisher pub(tb);
    BOOST_REQUIRE_EQUAL(tb.total_shares(), 0u);
    {
        satb<>::consumer cs1(pub, 100);
        // inactive by default
        BOOST_REQUIRE_EQUAL(tb.total_shares(), 0u);
        cs1.activate();
        // deferred: total_shares stays 0 until flush()
        BOOST_REQUIRE_EQUAL(tb.total_shares(), 0u);
        pub.flush();
        BOOST_REQUIRE_EQUAL(tb.total_shares(), 100u);
        {
            satb<>::consumer cs2(pub, 200);
            cs2.activate();
            // deferred
            BOOST_REQUIRE_EQUAL(tb.total_shares(), 100u);
            pub.flush();
            BOOST_REQUIRE_EQUAL(tb.total_shares(), 300u);
            cs2.update_shares(400);
            // deferred as well
            BOOST_REQUIRE_EQUAL(tb.total_shares(), 300u);
            pub.flush();
            BOOST_REQUIRE_EQUAL(tb.total_shares(), 500u);
            // destructor queues cs2's rollback into pub
        }
        pub.flush();
        BOOST_REQUIRE_EQUAL(tb.total_shares(), 100u);
        // destructor queues cs1's rollback into pub
    }
    pub.flush();
    BOOST_REQUIRE_EQUAL(tb.total_shares(), 0u);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_satb_tau_forgiveness) {
    // rate=100/s, burst=1000, tau=2s; class is sole competitor.
    // After consuming all tokens and idling for 10s, on re-activation the class
    // should only receive credit for tau=2s worth of tokens, not all 10s.
    satb<> tb(100, 1000, 1, false, 2s);
    satb<>::local_publisher pub(tb);
    satb<>::consumer cs(pub, 100);
    satb<>::tokens pouch(tb.limit());

    cs.activate();
    pub.flush();
    manual_clock::advance(1s);
    tb.replenish(manual_clock::now());
    // consume everything
    pouch.refill(cs.accrued());
    while (pouch.try_consume(1)) {}
    BOOST_REQUIRE_EQUAL(pouch.available(), 0u);

    cs.deactivate();
    pub.flush();

    // idle for 10 seconds
    for (int i = 0; i < 10; i++) {
        manual_clock::advance(1s);
        tb.replenish(manual_clock::now());
    }

    cs.activate();
    pub.flush();
    // tau=2s => at most 200 tokens of global credit, gain = 200*100/100 = 200
    // burst cap = 1000; so result is 200, not 1000
    pouch.refill(cs.accrued());
    BOOST_REQUIRE_EQUAL(pouch.available(), 200u);

    return make_ready_future<>();
}

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
