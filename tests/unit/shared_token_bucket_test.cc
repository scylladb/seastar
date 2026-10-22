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

#include <atomic>
#include <thread>

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

// With no other replenisher, every replenish that accrues tokens must land.
// A concurrent grab writes the cache line that holds the replenish timestamp,
// which on LL/SC targets (aarch64 without LSE) makes a weak CAS fail without
// any other writer of the timestamp, and so drop the replenish.
SEASTAR_TEST_CASE(test_replenish_not_lost_under_concurrent_grab) {
    using clock = std::chrono::steady_clock;
    alignas(64) internal::shared_token_bucket<uint64_t, std::ratio<1>, internal::capped_release::no, clock> tb(1'000'000, 1'000'000, 1, false);

    std::atomic<bool> stop{false};
    std::thread grabber([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            tb.grab(0);
        }
    });

    auto ts = tb.replenished_ts();
    unsigned lost = 0;
    for (int i = 0; i < 1'000'000; ++i) {
        ts += 1ms;
        tb.replenish(ts);
        lost += tb.replenished_ts() != ts;
    }
    stop.store(true, std::memory_order_relaxed);
    grabber.join();

    BOOST_REQUIRE_EQUAL(lost, 0u);
    return make_ready_future<>();
}

// Keep the accesses out of line so the race regression remains observable
// under compiler optimization.
template <typename Bucket>
[[gnu::noinline]] uint64_t observed_rate(const Bucket& tb) {
    return tb.rate();
}

template <typename Bucket>
[[gnu::noinline]] uint64_t observed_tokens(const Bucket& tb) {
    return tb.accumulated_in(1ms);
}

template <typename Bucket>
[[gnu::noinline]] double observed_duration(const Bucket& tb) {
    return tb.duration_for(1000).count();
}

// Rate changes can happen while other shards calculate token replenishment
// and throttling delays. These accesses must be safe concurrently.
SEASTAR_TEST_CASE(test_concurrent_rate_updates_and_reads) {
    using bucket_type = internal::shared_token_bucket<
        uint64_t, std::ratio<1>, internal::capped_release::no>;

    constexpr uint64_t slow_rate = 1'000'000;
    constexpr uint64_t fast_rate = 2'000'000;

    bucket_type tb(slow_rate, 4096, 1, false);

    std::atomic<bool> ready{false};
    std::atomic<bool> stop{false};
    std::atomic<unsigned> updates{0};

    std::thread updater([&] {
        uint64_t rate = fast_rate;
        tb.update_rate(rate);
        updates.fetch_add(1, std::memory_order_relaxed);
        ready.store(true, std::memory_order_release);

        while (!stop.load(std::memory_order_acquire)) {
            rate = rate == fast_rate ? slow_rate : fast_rate;
            tb.update_rate(rate);
            updates.fetch_add(1, std::memory_order_relaxed);
        }
    });

    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const auto updates_before = updates.load(std::memory_order_relaxed);
    bool valid = true;

    for (unsigned i = 0; i < 100'000; ++i) {
        if ((i & 1023) == 0) {
            std::this_thread::yield();
        }
        const auto rate = observed_rate(tb);
        const auto tokens = observed_tokens(tb);
        const auto duration = observed_duration(tb);

        valid &= rate == slow_rate || rate == fast_rate;
        valid &= tokens == 1000 || tokens == 2000;
        valid &= duration >= 0.0005 && duration <= 0.001;
    }

    const auto updates_after = updates.load(std::memory_order_relaxed);
    stop.store(true, std::memory_order_release);
    updater.join();

    BOOST_REQUIRE_GT(updates_after, updates_before);
    BOOST_REQUIRE(valid);
    return make_ready_future<>();
}
