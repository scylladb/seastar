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
#include <seastar/core/metrics_api.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/thread_impl.hh>
#include <seastar/util/later.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <atomic>
#include <chrono>
#include <ranges>

#include <sys/mman.h>

#ifndef SEASTAR_DEBUG

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

static void spin_user_hires(std::chrono::duration<double> how_much) {
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

// Triggers stalls by spinning with a specify "body" function
// which takes most of the spin time.
static void test_spin_with_body(const char* what, void_fn body) {
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

SEASTAR_THREAD_TEST_CASE(spin_in_userspace) {
    // a body which spends almost all of its time in userspace
    test_spin_with_body("userspace", [] { spin_user_hires(1ms); });
}

static void mmap_populate(size_t len) {
    void *p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, 0, 0);
    BOOST_REQUIRE(p != MAP_FAILED);
    BOOST_REQUIRE(munmap(p, len) == 0);
}

SEASTAR_THREAD_TEST_CASE(spin_in_kernel) {
    // a body which spends almost all of its time in the kernel
    // doing 128K mmaps
    test_spin_with_body("kernel", [] { mmap_populate(128 * 1024); });
}

#ifdef __x86_64__
// Spin for the given number of iterations with the stack pointer and the frame
// pointer pointing into a different stack, without telling the unwinder. This
// is the state of a hand-written stack switch, like glibc's swapcontext(),
// between loading the new stack pointer and finishing the switch: the unwind
// information of the running function doesn't describe it, so an unwinder
// interrupted here takes words of the new stack as return addresses.
[[gnu::noinline]] static void spin_on_other_stack(void* stack_top, uint64_t iterations) {
    asm volatile(
        "mov %%rsp, %%r12\n\t"
        "mov %%rbp, %%r13\n\t"
        "mov %1, %%rsp\n\t"
        "mov %1, %%rbp\n\t"
        "1:\n\t"
        "dec %0\n\t"
        "jnz 1b\n\t"
        "mov %%r12, %%rsp\n\t"
        "mov %%r13, %%rbp\n\t"
        : "+r"(iterations)
        : "r"(stack_top)
        : "r12", "r13", "cc", "memory");
}

// The number of stalls this shard's stall detector reported.
static uint64_t reported_stalls() {
    namespace smi = seastar::metrics::impl;
    auto values = smi::get_values();
    const auto& metadata = *values->metadata;
    for (size_t i = 0; i < metadata.size(); i++) {
        if (metadata[i].mf.name == "stall_detector_reported") {
            return values->values[i][0].i();
        }
    }
    BOOST_FAIL("stall_detector_reported metric not found");
    return 0;
}

// An application which switches stacks by itself marks the switch with
// thread_impl::begin_context_switch() and end_context_switch(), and then a
// stall in the middle of the switch is reported without a backtrace - a
// backtrace would follow the new stack's words as return addresses, and crash
// in the unwinder when one isn't a mapped address.
SEASTAR_THREAD_TEST_CASE(stall_in_application_stack_switch) {
    // A stack filled with a small value which isn't a mapped address, below
    // and above the point where we switch to it.
    std::vector<uintptr_t> stack(128 * 1024, 0xc1daa);
    auto stack_top = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(stack.data() + stack.size() / 2) & ~uintptr_t(15));
    // Find how many iterations take 100ms of CPU time (which is what the stall
    // detector measures), more than the threshold below. The calibration spin
    // is marked too, in case it is long enough to be reported as a stall.
    constexpr uint64_t calibration_iterations = 10'000'000;
    auto start = thread_cputime_clock::now();
    thread_impl::begin_context_switch();
    spin_on_other_stack(stack_top, calibration_iterations);
    thread_impl::end_context_switch();
    auto calibration_time = thread_cputime_clock::now() - start;
    uint64_t iterations = calibration_iterations * (std::chrono::duration<double>(100ms) / calibration_time);

    temporary_stall_detector_settings tsds(10ms);
    // The new threshold takes effect in the next task
    thread::yield();
    auto reported_before = reported_stalls();
    thread_impl::begin_context_switch();
    spin_on_other_stack(stack_top, iterations);
    thread_impl::end_context_switch();
    // We didn't crash, and the stall was reported
    BOOST_REQUIRE_GT(reported_stalls(), reported_before);
}
#endif

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
