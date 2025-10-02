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

#include <seastar/core/future.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/util/later.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/testing/perf_tests.hh>

// Benchmarks that test raw overhead of almost empty perf tests
// in all the basic variations.

namespace {
volatile int sink;
constexpr auto ITER_COUNT = 100;
struct fixture { };
auto loop(size_t count = ITER_COUNT) {
    for (size_t i = 0; i < count; i++) {
        perf_tests::do_not_optimize(i);
    }
    return count;
}
}

PERF_TEST(perf_tests, test_simple_1) { perf_tests::do_not_optimize(sink); }

PERF_TEST(perf_tests, test_simple_n) { return loop(); }

// do more work in 1 inner iteration to get a high instruction count to help
// see the variability in the measurements
PERF_TEST(perf_tests, test_simple_n_big) {
    loop(10000000);
    return 1;
}

PERF_TEST(perf_tests, test_ready_async_1) { return now(); }

PERF_TEST(perf_tests, test_ready_async_n) { return as_ready_future(loop()); }

PERF_TEST(perf_tests, test_unready_async_1) { return yield(); }

PERF_TEST(perf_tests, test_unready_async_n) {
    auto i = loop();
    return yield().then([=] { return i; });
};

PERF_TEST_F(fixture, test_fixture_1) { perf_tests::do_not_optimize(sink); }

PERF_TEST_F(fixture, test_fixture_n) { return loop(); }

PERF_TEST_C(fixture, test_coro_1) {
    // without the next line, compiler will optimize away the coroutine nature of
    // this function and compile/inline it as a regular function
    co_await coroutine::maybe_yield();
}

PERF_TEST_CN(fixture, test_coro_n) {
    co_await coroutine::maybe_yield();
    co_return loop();
}

PERF_TEST(perf_tests, test_empty) { }

PERF_TEST(perf_tests, test_timer_overhead) {
    constexpr auto TIMER_LOOPS = 1000;
    for (size_t i = 0; i < TIMER_LOOPS; i++) {
        perf_tests::start_measuring_time();
        perf_tests::stop_measuring_time();
    }
    return TIMER_LOOPS;
}

// The following tests run in order check that pre-run hooks are executed properly.

static int hook_1_count = 0, hook_2_count = 1;

PERF_PRE_RUN_HOOK([](const std::string& g, const std::string& c) {
    ++hook_1_count;
});

PERF_PRE_RUN_HOOK([](const std::string& g, const std::string& c) {
    ++hook_2_count;
});

PERF_TEST(hook_checker, hook_did_run_0) {
    // check in a subsequent test that the hook ran
    assert(hook_1_count > 0);
    assert(hook_2_count > 0);
}

