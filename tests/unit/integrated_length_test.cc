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

#define BOOST_TEST_MODULE integrated_length

#include <boost/test/unit_test.hpp>

#include <seastar/util/integrated-length.hh>

#include <chrono>
#include <cstdint>
#include <random>

using namespace std::chrono_literals;

namespace {

// A clock that only moves when the test says so
struct test_clock {
    using duration = std::chrono::nanoseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<test_clock, duration>;

    static inline duration current{0};

    static time_point now() noexcept {
        return time_point(current);
    }
    static void advance(duration d) noexcept {
        current += d;
    }
    static void reset() noexcept {
        current = duration(0);
    }
};

using integrator = seastar::util::integrated_length<unsigned short, test_clock>;

}

BOOST_AUTO_TEST_CASE(test_constant_value) {
    test_clock::reset();
    integrator il(7);
    test_clock::advance(10s);
    BOOST_REQUIRE_EQUAL(il.integral(), 70u);
    test_clock::advance(5s);
    BOOST_REQUIRE_EQUAL(il.integral(), 105u);
}

// The value must be integrated over the time it was held, not over the time
// before it was set. A burst that follows a long idle period must not be
// charged for that period.
BOOST_AUTO_TEST_CASE(test_change_after_idle_period) {
    test_clock::reset();
    integrator il;
    test_clock::advance(1000s);
    il = 1000;
    test_clock::advance(1s);
    il = 0;
    test_clock::advance(1000s);
    BOOST_REQUIRE_EQUAL(il.integral(), 1000u);
}

// Same as above, but with explicit checkpoints right after the changes, as
// a dispatch loop would do them. They must not change the result.
BOOST_AUTO_TEST_CASE(test_checkpoint_after_change_after_idle_period) {
    test_clock::reset();
    integrator il;
    test_clock::advance(1000s);
    il = 1000;
    il.checkpoint();
    test_clock::advance(1s);
    il = 0;
    il.checkpoint();
    test_clock::advance(1000s);
    il.checkpoint();
    BOOST_REQUIRE_EQUAL(il.integral(), 1000u);
}

BOOST_AUTO_TEST_CASE(test_increments_and_decrements) {
    test_clock::reset();
    integrator il;
    test_clock::advance(3s);
    il++;                       // 1 for 2s
    test_clock::advance(2s);
    ++il;                       // 2 for 4s
    test_clock::advance(4s);
    il--;                       // 1 for 1s
    test_clock::advance(1s);
    --il;                       // 0 for 100s
    test_clock::advance(100s);
    BOOST_REQUIRE_EQUAL(il.value(), 0);
    BOOST_REQUIRE_EQUAL(il.integral(), 2u + 8u + 1u);
}

// A value that doesn't change still contributes to the result
BOOST_AUTO_TEST_CASE(test_integral_includes_time_since_last_change) {
    test_clock::reset();
    integrator il;
    il = 3;
    test_clock::advance(4s);
    BOOST_REQUIRE_EQUAL(il.integral(), 12u);
    il.checkpoint();
    BOOST_REQUIRE_EQUAL(il.integral(), 12u);
    test_clock::advance(1s);
    BOOST_REQUIRE_EQUAL(il.integral(), 15u);
}

BOOST_AUTO_TEST_CASE(test_explicit_time_points) {
    test_clock::reset();
    integrator il;
    auto t = test_clock::now();
    il.set(4, t + 1s);
    il.add(2, t + 3s);
    il.sub(5, t + 4s);
    BOOST_REQUIRE_EQUAL(il.value(), 1);
    // 4 for 2s, 6 for 1s, 1 for 6s
    BOOST_REQUIRE_EQUAL(il.integral(t + 10s), 8u + 6u + 6u);
}

// A time point earlier than one already seen doesn't change the result
BOOST_AUTO_TEST_CASE(test_earlier_time_point_is_ignored) {
    test_clock::reset();
    integrator il(5);
    auto t = test_clock::now();
    il.checkpoint(t + 2s);
    il.checkpoint(t + 1s);
    il.set(5, t + 1s);
    BOOST_REQUIRE_EQUAL(il.integral(t + 2s), 10u);
    BOOST_REQUIRE_EQUAL(il.integral(t), 10u);
}

// The time that is finer than the resolution must be carried over to the
// following changes, not dropped on each of them
BOOST_AUTO_TEST_CASE(test_sub_resolution_time_is_not_lost) {
    test_clock::reset();
    seastar::util::integrated_length<unsigned, test_clock, std::chrono::microseconds> il(1000);
    for (int i = 0; i < 1'000'000; i++) {
        test_clock::advance(1500ns);
        il.checkpoint();
    }
    // 1000 for 1.5s
    BOOST_REQUIRE_EQUAL(il.integral(), 1500u);
}

// The time that is finer than the resolution must be integrated at the value
// that was held during it, not at the value that follows it
BOOST_AUTO_TEST_CASE(test_sub_resolution_time_uses_the_held_value) {
    test_clock::reset();
    seastar::util::integrated_length<unsigned, test_clock, std::chrono::microseconds> rising, falling(1000);
    for (int i = 0; i < 1'000'000; i++) {
        test_clock::advance(999ns);
        rising = 1000;
        falling = 0;
        test_clock::advance(1ns);
        rising = 0;
        falling = 1000;
    }
    // 1000 for 1ns, and 1000 for 999ns, a million times each
    BOOST_REQUIRE_EQUAL(rising.integral(), 1u);
    BOOST_REQUIRE_EQUAL(falling.integral(), 999u);
}

// Random values held for random durations, compared to the exact sum
BOOST_AUTO_TEST_CASE(test_random_values) {
    test_clock::reset();
    integrator il;
    auto seed = std::random_device{}();
    BOOST_TEST_MESSAGE("seed=" << seed);
    std::default_random_engine reng(seed);
    std::uniform_int_distribution<unsigned> qlen(0, 250);
    std::uniform_int_distribution<int> dur(1, 1000);

    uint64_t expected = 0; // in nanoseconds * units
    unsigned value = 0;
    for (int i = 0; i < 100'000; i++) {
        auto d = std::chrono::microseconds(dur(reng));
        test_clock::advance(d);
        expected += uint64_t(value) * std::chrono::nanoseconds(d).count();
        value = qlen(reng);
        il = value;
    }
    BOOST_REQUIRE_EQUAL(il.integral(), expected / 1'000'000'000);
}

// Same as above, but with values held for durations that are not whole
// multiples of the resolution
BOOST_AUTO_TEST_CASE(test_random_values_finer_than_resolution) {
    test_clock::reset();
    seastar::util::integrated_length<unsigned, test_clock, std::chrono::microseconds> il;
    auto seed = std::random_device{}();
    BOOST_TEST_MESSAGE("seed=" << seed);
    std::default_random_engine reng(seed);
    std::uniform_int_distribution<unsigned> qlen(0, 1'000'000);
    std::uniform_int_distribution<int> dur(1, 3000);

    uint64_t expected = 0; // in nanoseconds * units
    unsigned value = 0;
    for (int i = 0; i < 1'000'000; i++) {
        auto d = std::chrono::nanoseconds(dur(reng));
        test_clock::advance(d);
        expected += uint64_t(value) * d.count();
        value = qlen(reng);
        il = value;
    }
    BOOST_REQUIRE_EQUAL(il.integral(), expected / 1'000'000'000);
}

// Values above 65535 must be integrated exactly. The I/O queue's per-class
// request counts can exceed that.
BOOST_AUTO_TEST_CASE(test_counter_crosses_65536) {
    test_clock::reset();
    seastar::util::integrated_length<unsigned, test_clock> il;

    for (unsigned i = 0; i < 65'536; ++i) {
        il.add(1, test_clock::now());
    }

    BOOST_REQUIRE_EQUAL(il.value(), 65'536u);

    test_clock::advance(1s);
    BOOST_REQUIRE_EQUAL(il.integral(), 65'536u);

    il.sub(1, test_clock::now());
    BOOST_REQUIRE_EQUAL(il.value(), 65'535u);

    test_clock::advance(1s);
    BOOST_REQUIRE_EQUAL(il.integral(), 131'071u);
}

// Check the public template with a wider unsigned integer value type.
BOOST_AUTO_TEST_CASE(test_uint64_value_type) {
    test_clock::reset();
    seastar::util::integrated_length<std::uint64_t, test_clock> il;

    const std::uint64_t value = 1'000'000'000ULL;

    il.set(value, test_clock::now());
    test_clock::advance(1s);

    BOOST_REQUIRE_EQUAL(il.value(), value);
    BOOST_REQUIRE_EQUAL(il.integral(), value);

    il.sub(value, test_clock::now());
    test_clock::advance(1s);

    BOOST_REQUIRE_EQUAL(il.value(), 0ULL);
    BOOST_REQUIRE_EQUAL(il.integral(), value);
}
