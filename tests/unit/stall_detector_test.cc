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

#include <seastar/core/reactor.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <atomic>
#include <chrono>

using namespace seastar;
using namespace std::chrono_literals;

class temporary_stall_detector_settings {
    std::chrono::milliseconds _old_threshold;
    std::function<void ()> _old_report;
public:
    temporary_stall_detector_settings(std::chrono::duration<double> threshold, std::function<void ()> report)
            : _old_threshold(engine().get_blocked_reactor_notify_ms())
            , _old_report(engine().get_stall_detector_report_function()) {
        engine().update_blocked_reactor_notify_ms(std::chrono::duration_cast<std::chrono::milliseconds>(threshold));
        engine().set_stall_detector_report_function(std::move(report));
    }
    ~temporary_stall_detector_settings() {
        engine().update_blocked_reactor_notify_ms(_old_threshold);
        engine().set_stall_detector_report_function(std::move(_old_report));
    }
};

void spin(std::chrono::duration<double> how_much) {
    auto end = std::chrono::steady_clock::now() + how_much;
    while (std::chrono::steady_clock::now() < end) {
        // spin!
    }
}

void spin_some_cooperatively(std::chrono::duration<double> how_much) {
    auto end = std::chrono::steady_clock::now() + how_much;
    while (std::chrono::steady_clock::now() < end) {
        spin(200us);
        if (need_preempt()) {
            thread::yield();
        }
    }
}

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


