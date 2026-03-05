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

#define BOOST_TEST_MODULE linux_perf_event

#include <boost/test/unit_test.hpp>
#include <seastar/testing/linux_perf_event.hh>

BOOST_AUTO_TEST_CASE(test_always_zero_reads_zero) {
    auto event = linux_perf_event::always_zero();
    BOOST_CHECK_EQUAL(event.read(), 0u);
    BOOST_CHECK_EQUAL(event.read(), 0u);
}

BOOST_AUTO_TEST_CASE(test_always_zero_enable_disable) {
    auto event = linux_perf_event::always_zero();
    // These should be no-ops and not crash
    event.enable();
    event.disable();
    BOOST_CHECK_EQUAL(event.read(), 0u);
}

BOOST_AUTO_TEST_CASE(test_always_zero_move) {
    auto event = linux_perf_event::always_zero();
    auto event2 = std::move(event);
    BOOST_CHECK_EQUAL(event2.read(), 0u);

    linux_perf_event event3 = linux_perf_event::always_zero();
    event3 = std::move(event2);
    BOOST_CHECK_EQUAL(event3.read(), 0u);
}
