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

#include "test-utils.hh"

#include <core/do_with.hh>
#include <core/lowres_clock.hh>
#include <core/sleep.hh>

#include <ctime>

#include <algorithm>
#include <array>
#include <chrono>

using namespace seastar;

//
// Sanity check the accuracy of the steady low-resolution clock.
//
SEASTAR_TEST_CASE(steady_clock_sanity) {
    static constexpr auto sleep_duration = std::chrono::milliseconds(100);

    return do_with(lowres_clock::now(), [](auto &&t1) {
        return ::seastar::sleep(sleep_duration).then([&t1] {
            auto const elapsed = lowres_clock::now() - t1;
            auto const minimum_elapsed = 0.9 * sleep_duration;

            BOOST_REQUIRE(elapsed >= (0.9 * sleep_duration));

            return make_ready_future<>();
        });
    });
}
