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
 * Copyright (C) 2018 ScyllaDB Ltd.
 */

#include <boost/test/tools/old/interface.hpp>
#include <cstddef>
#include <seastar/core/internal/stall_detector.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/thread_cputime_clock.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/later.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <atomic>
#include <chrono>
#include <sys/mman.h>

#ifndef SEASTAR_DEBUG
#include "stall_detector_test_utilities.hh"

SEASTAR_THREAD_TEST_CASE(normal_case) {
    std::atomic<unsigned> reports{};
    temporary_stall_detector_settings tsds(10ms, [&] { ++reports; });
    spin_some_cooperatively(1s);
    BOOST_REQUIRE_EQUAL(reports, 0);
}

SEASTAR_THREAD_TEST_CASE(simple_stalls) {
    std::atomic<unsigned> reports{};
    temporary_stall_detector_settings tsds(10ms, [&] { ++reports; });
    unsigned nr = 10;
    for (unsigned i = 0; i < nr; ++i) {
        spin_some_cooperatively(100ms);
        spin(20ms);
    }
    spin_some_cooperatively(100ms);

    // blocked-reactor-reports-per-minute defaults to 5, so we don't
    // get all 10 reports.
    BOOST_REQUIRE_EQUAL(reports, 5);
}

SEASTAR_THREAD_TEST_CASE(no_poll_no_stall) {
    std::atomic<unsigned> reports{};
    temporary_stall_detector_settings tsds(10ms, [&] { ++reports; });
    spin_some_cooperatively(1ms); // need to yield so that stall detector change from above take effect
    static constexpr unsigned tasks = 2000;
    promise<> p;
    auto f = p.get_future();
    parallel_for_each(boost::irange(0u, tasks), [&p] (unsigned int i) {
        (void)yield().then([i, &p] {
            spin(500us);
            if (i == tasks - 1) {
                p.set_value();
            }
        });
        return make_ready_future<>();
    }).get();
    f.get();
    BOOST_REQUIRE_EQUAL(reports, 0);
}

SEASTAR_THREAD_TEST_CASE(spin_in_userspace) {
    // a body which spends almost all of its time in userspace
    test_spin_with_body("userspace", [] { spin_user_hires(1ms); });
}

SEASTAR_THREAD_TEST_CASE(spin_in_kernel) {
    // a body which spends almost all of its time in the kernel
    // doing 128K mmaps
    test_spin_with_body("kernel", [] { mmap_populate(128 * 1024); });
}


#else

SEASTAR_THREAD_TEST_CASE(stall_detector_test_not_valid_in_debug_mode) {
}

#endif
