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

#ifdef SEASTAR_COROUTINES_ENABLED

#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/maybe_yield.hh>

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

#endif // SEASTAR_COROUTINES_ENABLED
