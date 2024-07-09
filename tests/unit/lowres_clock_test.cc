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
 * Copyright (C) 2017 ScyllaDB
 */

#include <seastar/testing/test_case.hh>

#include <seastar/core/coroutine.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/later.hh>

#include <ctime>

#include <chrono>
#include <thread>

using namespace seastar;

//
// Sanity check the accuracy of the steady low-resolution clock.
//
SEASTAR_TEST_CASE(steady_clock_sanity) {
    return do_with(lowres_clock::now(), [](auto &&t1) {
        static constexpr auto sleep_duration = std::chrono::milliseconds(100);

        return ::seastar::sleep(sleep_duration).then([&t1] {
            auto const elapsed = lowres_clock::now() - t1;
            auto const minimum_elapsed = 0.9 * sleep_duration;

            BOOST_REQUIRE(elapsed >= minimum_elapsed);

            return make_ready_future<>();
        });
    });
}

//
// Sanity check the accuracy of the steady low-resolution clock in debug mode
// where preemption is handled differently.
//
SEASTAR_TEST_CASE(steady_clock_sanity_preempt) {
    auto t1 = lowres_clock::now();

    // Sleep duration must be higher than the task quota to ensure that
    // preemption is requested before we yield.
    static constexpr auto sleep_duration = std::chrono::milliseconds(100);
    std::this_thread::sleep_for(sleep_duration);

    // Yield to the reactor to give it a chance to update the
    // low-resolution clock. Yield just schedules an empty task and
    // waits for it to be executed. There is nothing special about it.
    co_await ::seastar::yield();

    auto const elapsed = lowres_clock::now() - t1;
    auto const minimum_elapsed = 0.9 * sleep_duration;

    BOOST_REQUIRE(elapsed >= minimum_elapsed);
}

//
// At the very least, we can verify that the low-resolution system clock is within a second of the
// high-resolution system clock.
//
SEASTAR_TEST_CASE(system_clock_sanity) {
    static const auto check_matching = [] {
        auto const system_time = std::chrono::system_clock::now();
        auto const lowres_time = lowres_system_clock::now();

        auto const t1 = std::chrono::system_clock::to_time_t(system_time);
        auto const t2 = lowres_system_clock::to_time_t(lowres_time);

        std::tm *lt1 = std::localtime(&t1);
        std::tm *lt2 = std::localtime(&t2);

        return (lt1->tm_isdst == lt2->tm_isdst) &&
               (lt1->tm_year == lt2->tm_year) &&
               (lt1->tm_mon == lt2->tm_mon) &&
               (lt1->tm_yday == lt2->tm_yday) &&
               (lt1->tm_mday == lt2->tm_mday) &&
               (lt1->tm_wday == lt2->tm_wday) &&
               (lt1->tm_hour == lt2->tm_hour) &&
               (lt1->tm_min == lt2->tm_min) &&
               (lt1->tm_sec == lt2->tm_sec);
    };

    //
    // Check two out of three samples in order to account for the possibility that the high-resolution clock backing
    // the low-resoltuion clock was captured in the range of the 990th to 999th millisecond of the second. This would
    // make the low-resolution clock and the high-resolution clock disagree on the current second.
    //

    return do_with(0ul, 0ul, [](std::size_t& index, std::size_t& success_count) {
        return repeat([&index, &success_count] {
            if (index >= 3) {
                BOOST_REQUIRE_GE(success_count, 2u);
                return make_ready_future<stop_iteration>(stop_iteration::yes);
            }

            return ::seastar::sleep(std::chrono::milliseconds(10)).then([&index, &success_count] {
                if (check_matching()) {
                    ++success_count;
                }

                ++index;
                return stop_iteration::no;
            });
        });
    });
}

//
// Verify that the low-resolution clock updates its reported time point over time.
//
SEASTAR_TEST_CASE(system_clock_dynamic) {
    return do_with(lowres_system_clock::now(), [](auto &&t1) {
        return seastar::sleep(std::chrono::milliseconds(100)).then([&t1] {
            auto const t2 = lowres_system_clock::now();
            BOOST_REQUIRE_NE(t1.time_since_epoch().count(), t2.time_since_epoch().count());

            return make_ready_future<>();
        });
    });
}
