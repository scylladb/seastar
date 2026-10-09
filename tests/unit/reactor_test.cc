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

#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/timer.hh>
#include <seastar/testing/test_case.hh>

#include <chrono>
#include <random>

using namespace seastar;
using namespace std::chrono_literals;

namespace {

struct busy_time_sampler {
    steady_clock_type::duration last = engine().total_busy_time();
    unsigned samples = 0;
    unsigned decreases = 0;
    steady_clock_type::duration max_decrease{};

    void sample() {
        auto busy = engine().total_busy_time();
        ++samples;
        if (busy < last) {
            ++decreases;
            max_decrease = std::max(max_decrease, last - busy);
        }
        last = busy;
    }
};

void spin_for(steady_clock_type::duration d) {
    auto start = steady_clock_type::now();
    while (steady_clock_type::now() - start < d) {
    }
}

}

// total_busy_time() is exported as a counter, so it must never decrease. Sample
// it from code that runs while the reactor is idle or right after it wakes up:
// timer callbacks and a task resumed by a sleep, with sleeps of varying length
// so the idle periods preceding each sample differ.
SEASTAR_TEST_CASE(test_busy_time_never_decreases) {
    busy_time_sampler sampler;
    timer<> t([&sampler] { sampler.sample(); });
    t.arm_periodic(3ms);
    timer<lowres_clock> lowres_t([&sampler] { sampler.sample(); });
    lowres_t.arm_periodic(20ms);

    auto seed = std::random_device{}();
    BOOST_TEST_MESSAGE(fmt::format("seed {}", seed));
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> sleep_us(0, 5000);
    auto deadline = steady_clock_type::now() + 2500ms;
    while (steady_clock_type::now() < deadline) {
        co_await seastar::sleep(std::chrono::microseconds(sleep_us(rng)));
        sampler.sample();
    }
    t.cancel();
    lowres_t.cancel();

    BOOST_TEST_MESSAGE(fmt::format("{} samples, {} decreases, max decrease {}us",
            sampler.samples, sampler.decreases, sampler.max_decrease / 1us));
    BOOST_REQUIRE_EQUAL(sampler.decreases, 0);
}

// Work done by a task woken up from sleep is busy time, even when the reactor
// finds nothing else to do before sleeping again, and the sleeps are not.
SEASTAR_TEST_CASE(test_busy_time_counts_work_after_wakeup) {
    // Take the readings after a cross-shard round trip, which ends any idle
    // period, so they do not depend on how busy time reads while idle.
    auto settle = [] { return smp::submit_to((this_shard_id() + 1) % smp::count, [] {}); };
    co_await settle();
    auto busy_before = engine().total_busy_time();
    steady_clock_type::duration spun{};
    steady_clock_type::duration slept{};
    for (int i = 0; i < 100; ++i) {
        auto sleep_start = steady_clock_type::now();
        co_await seastar::sleep(2ms);
        slept += steady_clock_type::now() - sleep_start;
        auto start = steady_clock_type::now();
        spin_for(500us);
        spun += steady_clock_type::now() - start;
    }
    co_await settle();
    auto busy = engine().total_busy_time() - busy_before;

    BOOST_TEST_MESSAGE(fmt::format("spun {}us, slept {}us, busy {}us",
            spun / 1us, slept / 1us, busy / 1us));
    BOOST_REQUIRE_GE(busy, spun);
    BOOST_REQUIRE_LT(busy, spun + slept / 2);
}
