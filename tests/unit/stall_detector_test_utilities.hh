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
 * Copyright (C) 2023 ScyllaDB Ltd.
 */

#pragma once

#include <cstddef>
#include <seastar/core/internal/stall_detector.hh>
#include <seastar/core/reactor.hh>
#include "seastar/core/scheduling.hh"
#include "seastar/core/thread.hh"
#include "seastar/coroutine/maybe_yield.hh"
#include <seastar/core/thread_cputime_clock.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/later.hh>
#include <atomic>
#include <chrono>
#include <sys/mman.h>
#include <boost/test/tools/old/interface.hpp>

namespace {

using namespace seastar;
using namespace std::chrono_literals;

static seastar::logger testlog("testlog");

class temporary_stall_detector_settings {
    std::chrono::milliseconds _old_threshold;
    std::function<void ()> _old_report;
public:
    /**
     * Temporarily (until destructor) overload the stall detector threshold and reporting function.
     *
     * Also resets the reported stalls counter to zero, so the next backtraces will not be supressed.
     */
    temporary_stall_detector_settings(std::chrono::duration<double> threshold, std::function<void ()> report = {})
            : _old_threshold(engine().get_blocked_reactor_notify_ms())
            , _old_report(reactor::test::get_stall_detector_report_function()) {
        engine().update_blocked_reactor_notify_ms(std::chrono::duration_cast<std::chrono::milliseconds>(threshold));
        reactor::test::set_stall_detector_report_function(std::move(report));
    }

    ~temporary_stall_detector_settings() {
        engine().update_blocked_reactor_notify_ms(_old_threshold);
        reactor::test::set_stall_detector_report_function(std::move(_old_report));
    }
};

using void_fn = std::function<void()>;

void spin(std::chrono::duration<double> how_much, void_fn body = []{}) {
    auto end = internal::cpu_stall_detector::clock_type::now() + how_much;
    while (internal::cpu_stall_detector::clock_type::now() < end) {
        body(); // spin!
    }
}

// Function unused in debug mode
[[maybe_unused]] void spin_user_hires(std::chrono::duration<double> how_much) {
    auto end = std::chrono::high_resolution_clock::now() + how_much;
    while (std::chrono::high_resolution_clock::now() < end) {

    }
}

void spin_some_cooperatively(std::chrono::duration<double> how_much, void_fn body = []{}) {
    auto end = std::chrono::steady_clock::now() + how_much;
    while (std::chrono::steady_clock::now() < end) {
        spin(200us, body);
        if (need_preempt()) {
            thread::yield();
        }
    }
}

future<> spin_some_cooperatively_coro(std::chrono::duration<double> how_much, void_fn body = []{}) {
    auto end = std::chrono::steady_clock::now() + how_much;
    while (std::chrono::steady_clock::now() < end) {
        // fmt::print("GC: {}\n", current_scheduling_group().name());
        spin(200us, body);
        co_await coroutine::maybe_yield();
    }
}


// Triggers stalls by spinning with a specify "body" function
// which takes most of the spin time.
inline void test_spin_with_body(const char* what, void_fn body) {
    // The !count_stacks mode outputs stall notification to stderr as usual
    // and do not assert anything, but are intended for diagnosing
    // stall problems by inspecting the output. We expect the userspace
    // spin test to show no kernel callstack, and the kernel test to
    // show kernel backtraces in the mmap or munmap path, but this is
    // not exact since neither test spends 100% of its time in the
    // selected mode (of course, kernel stacks only appear if the
    // perf-based stall detected could be enabled).
    //
    // Then the count_stacks mode tests that the right number of stacks
    // were output.
    for (auto count_stacks : {false, true}) {
        testlog.info("Starting spin test: {}", what);
        std::atomic<unsigned> reports{};
        std::function<void()> reporter = count_stacks ? std::function<void()>{[&]{ ++reports; }} : nullptr;
        temporary_stall_detector_settings tsds(10ms, std::move(reporter));
        constexpr unsigned nr = 5;
        for (unsigned i = 0; i < nr; ++i) {
            spin_some_cooperatively(100ms, body);
            spin(20ms, body);
        }
        testlog.info("Ending spin test: {}", what);
        BOOST_CHECK_EQUAL(reports, count_stacks ? 5 : 0);
    }
}

void mmap_populate(size_t len) {
    void *p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
    BOOST_REQUIRE(p != MAP_FAILED);
    BOOST_REQUIRE(munmap(p, len) == 0);
}

} // namespace
