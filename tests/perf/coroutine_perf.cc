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
 * Copyright (C) 2022-present ScyllaDB
 */

#include <seastar/testing/perf_tests.hh>

#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/coroutine/generator.hh>
#include <seastar/core/circular_buffer_fixed_capacity.hh>
#include <seastar/util/later.hh>
#include <vector>

struct coroutine_test {
};

PERF_TEST_C(coroutine_test, empty)
{
    co_return;
}

PERF_TEST_C(coroutine_test, without_preemption_check)
{
    co_await coroutine::without_preemption_check(make_ready_future<>());
}

PERF_TEST_C(coroutine_test, ready)
{
    co_await make_ready_future<>();
}

PERF_TEST_C(coroutine_test, maybe_yield)
{
    co_await coroutine::maybe_yield();
}

// Benchmark unbuffered generator: one suspension per element
PERF_TEST_C(coroutine_test, unbuffered_generator)
{
    constexpr int count = 100;

    auto gen = []() -> coroutine::experimental::generator<int> {
        for (int i = 0; i < count; ++i) {
            co_yield i;
        }
    }();

    int sum = 0;
    while (auto val = co_await gen()) {
        sum += *val;
    }
    perf_tests::do_not_optimize(sum);
}

// Benchmark buffered generator: amortized suspension overhead
PERF_TEST_C(coroutine_test, buffered_generator)
{
    constexpr int count = 100;
    constexpr int buffer_size = 16;

    auto gen = []() -> coroutine::experimental::generator<int, int, circular_buffer_fixed_capacity<int, buffer_size>> {
        for (int i = 0; i < count; ++i) {
            co_yield i;
        }
    }();

    int sum = 0;
    while (auto val = co_await gen()) {
        sum += *val;
    }
    perf_tests::do_not_optimize(sum);
}
