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
#include <ranges>

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
    parallel_for_each(std::views::iota(0u, tasks), [&p] (unsigned int i) {
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

// This test reproduces the issue described in https://github.com/scylladb/seastar/issues/2697
// The issue is reproduced most quickly using the following arguments:
// --blocked-reactor-notify-ms=1
// but it will happen with default arguments as well.
SEASTAR_THREAD_TEST_CASE(stall_detector_crash) {
    // increase total_iters to increase chance of failure
    // the value below is tuned to take about 1 second in
    // release builds
    constexpr auto total_iters = 100000;
    constexpr int max_depth = 20;

    auto now = [] { return std::chrono::high_resolution_clock::now(); };

    auto recursive_thrower = [](auto self, int x) -> void {
        if (x <= 0) {
            throw std::runtime_error("foo");
        } else {
            try {
                self(self, x - 1);
            } catch (...) {
                if (x & 0xF) {
                    throw;
                }
            }
        }
    };

    auto next_yield = now();
    for (int a = 1; a < total_iters; a++) {
        if (now() > next_yield) {
            // we need to periodically yield or else the stall reports will become
            // less and less frequent as exponentially grow the report interval while
            // the same task is running
            thread::yield();
            next_yield = now() + 40ms;
            // the next line resets the suppression state which allows many more reports
            // per second, increasing the chance of a failure
            reactor::test::set_stall_detector_report_function({});
        }

        try {
            recursive_thrower(recursive_thrower, a % max_depth);
        } catch (...) {
        }

        if (a % 100000 == 0) {
            fmt::print("Making progress: {:6.3f}%\n", 100. * a / total_iters);
        }
  }
}



#else

SEASTAR_THREAD_TEST_CASE(stall_detector_test_not_valid_in_debug_mode) {
}

#endif
