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
auto loop() {
    for (size_t i = 0; i < ITER_COUNT; i++) {
        perf_tests::do_not_optimize(i);
    }
    return ITER_COUNT;
}
}

PERF_TEST(perf_tests, test_simple_1) { perf_tests::do_not_optimize(sink); }

PERF_TEST(perf_tests, test_simple_n) { return loop(); }

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
