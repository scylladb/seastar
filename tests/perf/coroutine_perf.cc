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
#include <seastar/coroutine/as_future.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/coroutine/try_future.hh>
#include <seastar/coroutine/generator.hh>
#include <seastar/core/circular_buffer_fixed_capacity.hh>
#include <seastar/util/later.hh>
#include <string>
#include <vector>

using namespace seastar;

struct coroutine_test {
};

static future<int> ready_chain_leaf(int x) {
    co_return x + 1;
}

static future<int> ready_chain_middle(int x) {
    co_return co_await ready_chain_leaf(x);
}

static future<int> ready_chain_top(int x) {
    co_return co_await ready_chain_middle(x);
}

static future<int> wrapped_ready_chain_top(int x) {
    auto ready = co_await coroutine::as_future(ready_chain_middle(x));
    co_return ready.get() + co_await coroutine::try_future(make_ready_future<int>(1));
}

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

PERF_TEST_C(coroutine_test, nested_ready_chain)
{
    auto value = co_await ready_chain_top(0);
    perf_tests::do_not_optimize(value);
}

PERF_TEST_C(coroutine_test, wrapped_ready_chain)
{
    auto value = co_await wrapped_ready_chain_top(0);
    perf_tests::do_not_optimize(value);
}

PERF_TEST_C(coroutine_test, maybe_yield)
{
    co_await coroutine::maybe_yield();
}

namespace {

static constexpr size_t CORO_BENCH_ITERS = 10000;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static bool always_false_global = false;

bool always_false() {
    perf_tests::do_not_optimize(always_false_global);
    return always_false_global;
}

template<typename F>
future<size_t> co_await_in_loop(F f) {
    perf_tests::start_measuring_time();
    for (size_t i = 0; i < CORO_BENCH_ITERS; i++) {
        co_await f();
    }
    perf_tests::stop_measuring_time();
    co_return CORO_BENCH_ITERS;
}

template<typename F>
future<size_t> collect_futures(F f) {
    std::vector<future<>> futs;
    futs.reserve(CORO_BENCH_ITERS);
    perf_tests::start_measuring_time();
    for (size_t i = 0; i < CORO_BENCH_ITERS; i++) {
        futs.emplace_back(f());
    }
    perf_tests::stop_measuring_time();

    for (auto& fut : futs) {
        co_await std::move(fut);
    }
    co_return CORO_BENCH_ITERS;
}

[[gnu::noinline]] future<> bench_ss_now() { return now(); }

[[gnu::noinline]] future<> bench_empty_cont() { return maybe_yield(); }

[[gnu::noinline]] static auto bench_ready_future_int() {
    return make_ready_future<int>(1);
}

future<int> always_ready_coro() {
    int x = 0;
    if (always_false()) {
        x = co_await coroutine::without_preemption_check(
          bench_ready_future_int());
    }
    co_return x;
}

inline future<> always_ready() {
    if (always_false()) [[unlikely]] {
        return yield();
    }
    return now();
}

template<typename T>
inline future<T> always_ready_val(T&& t) {
    if (always_false()) [[unlikely]] {
        return yield().then([=]() mutable { return std::move(t); });
    }
    return make_ready_future<T>(std::forward<T>(t));
}

future<> never_awaits() {
    if (always_false()) {
        co_await make_ready_future<>();
    }
}

volatile int some_int;

void do_work() { some_int = 1; }

template<typename T>
auto do_work_val(T& t) {
    return always_ready_val(std::move(t));
}

inline future<> co_await_ready() {
    co_await always_ready();
    do_work();
}

template<size_t depth>
inline future<> co_await_ready_nested() {
    static_assert(depth > 0);

    if constexpr (depth == 1) {
        co_await always_ready();
    } else {
        co_await co_await_ready_nested<depth - 1>();
    }
    do_work();
}

struct small_object {
    char x;
};

future<> nested_after_yield() {
    using T = small_object;
    T t{};
    return yield().then([t = t]() mutable {
        return do_work_val(t)
          .then([](T t) { return do_work_val(t); })
          .then([](T t) { return do_work_val(t); })
          .then([](T t) { return do_work_val(t); })
          .then([](T t) { return do_work_val(t); })
          .then([](T) { return always_ready(); });
    });
}

future<> chained_after_yield() {
    using T = small_object;
    T t{};
    return yield()
      .then([t = t]() mutable { return do_work_val(t); })
      .then([](T t) { return do_work_val(t); })
      .then([](T t) { return do_work_val(t); })
      .then([](T t) { return do_work_val(t); })
      .then([](T t) { return do_work_val(t); })
      .then([](T) { return always_ready(); })
      .finally([] {})
      .finally([] {});
}

future<> coro_after_yield() {
    small_object t{};
    co_await yield();

    t = co_await do_work_val(t);
    t = co_await do_work_val(t);
    t = co_await do_work_val(t);
    t = co_await do_work_val(t);
    t = co_await do_work_val(t);
}

future<> chain_then5() {
    using T = std::string;
    T t{};
    return do_work_val(t)
      .then([](T t) { return t; })
      .then([](T t) { return t; })
      .then([](T t) { return t; })
      .then([](T t) { return t; })
      .then([](T t) { return t; })
      .then([](T) { return always_ready(); });
}

inline future<> nested_then5() {
    return always_ready().then([] {
        return always_ready().then([] {
            return always_ready().then([] {
                return always_ready().then(
                  [] { return always_ready().then([] { do_work(); }); });
            });
        });
    });
}

// Per-field coroutine: small, completes synchronously, body visible to the
// caller.  With std::suspend_never at final_suspend the compiler can prove
// bounded lifetime and stack-allocate (standard frame elision).  A custom
// final awaiter that suspends blocks that proof.
[[gnu::noinline]] future<int> per_field_process(int v) {
    perf_tests::do_not_optimize(v);
    co_return v + 1;
}

// Processes N fields in a loop, co_awaiting a directly-visible coroutine
// per field.
template<size_t NFields>
future<size_t> co_await_per_field_direct() {
    perf_tests::start_measuring_time();
    for (size_t record = 0; record < CORO_BENCH_ITERS; ++record) {
        int acc = 0;
        for (size_t i = 0; i < NFields; ++i) {
            acc = co_await per_field_process(acc);
        }
        perf_tests::do_not_optimize(acc);
    }
    perf_tests::stop_measuring_time();
    co_return CORO_BENCH_ITERS;
}

} // anonymous namespace

PERF_TEST_F(coroutine_test, cont_empty) { return co_await_in_loop(bench_empty_cont); }

PERF_TEST_F(coroutine_test, cont_ss_now) { return co_await_in_loop(bench_ss_now); }

PERF_TEST_F(coroutine_test, cont_ss_now_collect) { return collect_futures(bench_ss_now); }

PERF_TEST_F(coroutine_test, cont_nested_then5) { return co_await_in_loop(nested_then5); }

PERF_TEST_F(coroutine_test, cont_chain_then5) { return co_await_in_loop(chain_then5); }

PERF_TEST_F(coroutine_test, cont_nested_after_yield) { return co_await_in_loop(nested_after_yield); }

PERF_TEST_F(coroutine_test, cont_chained_after_yield) { return co_await_in_loop(chained_after_yield); }

PERF_TEST_F(coroutine_test, coro_ready) { return co_await_in_loop(co_await_ready); }

PERF_TEST_F(coroutine_test, coro_ready_collect) { return collect_futures(co_await_ready); }

PERF_TEST_F(coroutine_test, coro_after_yield) { return co_await_in_loop(coro_after_yield); }

PERF_TEST_F(coroutine_test, coro_ready_nested3) { return co_await_in_loop(co_await_ready_nested<3>); }

PERF_TEST_F(coroutine_test, coro_ready_nested5) { return co_await_in_loop(co_await_ready_nested<5>); }

PERF_TEST_F(coroutine_test, coro_empty) { return co_await_in_loop(always_ready_coro); }

PERF_TEST_F(coroutine_test, coro_never_awaits_collect) { return collect_futures(never_awaits); }

PERF_TEST_F(coroutine_test, coro_per_field_1) { return co_await_per_field_direct<1>(); }

PERF_TEST_F(coroutine_test, coro_per_field_10) { return co_await_per_field_direct<10>(); }

PERF_TEST_F(coroutine_test, coro_per_field_40) { return co_await_per_field_direct<40>(); }

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
