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

#pragma once

#include <atomic>
#include <memory>

#include <fmt/format.h>

#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/loop.hh>
#include <seastar/testing/linux_perf_event.hh>
#include <seastar/util/noncopyable_function.hh>

using namespace seastar;

namespace perf_tests {

// The type of the pre-run hook. See PERF_PRE_RUN_HOOK.
using pre_run_hook = noncopyable_function<void(const sstring& test_group, const sstring& test_case)>;

namespace internal {

struct config;

using clock_type = std::chrono::steady_clock;

class perf_stats {
public:
    uint64_t allocations = 0;
    uint64_t tasks_executed = 0;
    uint64_t instructions_retired = 0;
    uint64_t cpu_cycles_retired = 0;

private:
    static uint64_t perf_mallocs();
    static uint64_t perf_tasks_processed();

public:
    perf_stats() = default;
    perf_stats(uint64_t allocations_, uint64_t tasks_executed_, uint64_t instructions_retired_ = 0, uint64_t cpu_cycles_retired_ = 0)
        : allocations(allocations_)
        , tasks_executed(tasks_executed_)
        , instructions_retired(instructions_retired_)
        , cpu_cycles_retired(cpu_cycles_retired_)
    {}
    perf_stats(perf_stats&& o) noexcept
        : allocations(std::exchange(o.allocations, 0))
        , tasks_executed(std::exchange(o.tasks_executed, 0))
        , instructions_retired(std::exchange(o.instructions_retired, 0))
        , cpu_cycles_retired(std::exchange(o.cpu_cycles_retired, 0))
    {}
    perf_stats(const perf_stats& o) = default;

    perf_stats& operator=(perf_stats&& o) = default;
    perf_stats& operator=(const perf_stats& o) = default;

    perf_stats& operator+=(perf_stats b);
    perf_stats& operator-=(perf_stats b);

    static perf_stats snapshot(linux_perf_event* instructions_retired_counter = nullptr, linux_perf_event* cpu_cycles_retired_counter = nullptr);
};

inline
perf_stats
operator+(perf_stats a, perf_stats b) {
    a.allocations += b.allocations;
    a.tasks_executed += b.tasks_executed;
    a.instructions_retired += b.instructions_retired;
    a.cpu_cycles_retired += b.cpu_cycles_retired;
    return a;
}

inline
perf_stats
operator-(perf_stats a, perf_stats b) {
    a.allocations -= b.allocations;
    a.tasks_executed -= b.tasks_executed;
    a.instructions_retired -= b.instructions_retired;
    a.cpu_cycles_retired -= b.cpu_cycles_retired;
    return a;
}

inline perf_stats& perf_stats::operator+=(perf_stats b) {
    allocations += b.allocations;
    tasks_executed += b.tasks_executed;
    instructions_retired += b.instructions_retired;
    cpu_cycles_retired += b.cpu_cycles_retired;
    return *this;
}

inline perf_stats& perf_stats::operator-=(perf_stats b) {
    allocations -= b.allocations;
    tasks_executed -= b.tasks_executed;
    instructions_retired -= b.instructions_retired;
    cpu_cycles_retired -= b.cpu_cycles_retired;
    return *this;
}

class performance_test {
    std::string _test_case;
    std::string _test_group;

    uint64_t _single_run_iterations = 0;
    std::atomic<uint64_t> _max_single_run_iterations;
private:
    void do_run(const config&);
public:
    struct run_result {
        clock_type::duration duration;
        perf_stats stats;
        uint64_t start_stop_count = 0;
    };
protected:
    [[gnu::always_inline]] [[gnu::hot]]
    bool stop_iteration() const {
        return _single_run_iterations >= _max_single_run_iterations.load(std::memory_order_relaxed);
    }

    [[gnu::always_inline]] [[gnu::hot]]
    void next_iteration(size_t n) {
        _single_run_iterations += n;
    }

    virtual void set_up() = 0;
    virtual void tear_down() noexcept = 0;
    virtual future<run_result> do_single_run() = 0;

    // execute the pre-run hooks
    void run_hooks();

    // start/stop timing for a single run - called by do_single_run()
    void start_run();
    run_result stop_run();
public:
    performance_test(const std::string& test_case, const std::string& test_group)
        : _test_case(test_case)
        , _test_group(test_group)
    { }

    virtual ~performance_test() = default;

    const std::string& test_case() const { return _test_case; }
    const std::string& test_group() const { return _test_group; }
    std::string name() const { return fmt::format("{}.{}", test_group(), test_case()); }

    void run(const config&);
public:
    static void register_test(std::unique_ptr<performance_test>);
};

// Functions for time measurement iteration control - implementation in perf_tests.cc
void time_measurement_start_iteration();
void time_measurement_stop_iteration();

template<typename Test>
class concrete_performance_test final : public performance_test {
    std::optional<Test> _test;

    using test_ret_type = decltype(_test->run());
    // true iff the test method returns future<...>
    static constexpr bool is_async_test = is_future<test_ret_type>::value;
    // true iff the test returns the number of iterations run, otherwise it returns
    // void and we consider each invocation to be 1 iteration
    static constexpr bool is_iteration_returning = !(std::is_same_v<test_ret_type, future<>> || std::is_void_v<test_ret_type>);
private:

protected:
    virtual void set_up() override {
        _test.emplace();
    }

    virtual void tear_down() noexcept override {
        _test = std::nullopt;
    }

    [[gnu::hot]]
    virtual future<run_result> do_single_run() override {
        run_hooks();
        start_run();
        while (!stop_iteration()) {
            if constexpr (is_async_test) {
                if constexpr (is_iteration_returning) {
                    auto f = _test->run();
                    next_iteration(f.available() ? std::move(f).get() : co_await std::move(f));
                } else {
                    auto f = _test->run();
                    // The available() check is functionally redundant, but is significantly faster
                    // than invoking the co_await machinery on a future-returning function.
                    if (!f.available()) {
                        co_await std::move(f);
                    }
                    next_iteration(1);
                }
            } else {
                if constexpr (is_iteration_returning) {
                    next_iteration(_test->run());
                } else {
                    _test->run();
                    next_iteration(1);
                }
            }
        }
        co_return stop_run();
    }
public:
    using performance_test::performance_test;
};

template<typename Test>
struct test_registrar {
    test_registrar(const std::string& test_group, const std::string& test_case) {
        auto test = std::make_unique<concrete_performance_test<Test>>(test_case, test_group);
        performance_test::register_test(std::move(test));
    }
};


// internal hook registration function
int register_pre_run_hook(pre_run_hook hook);

}

[[gnu::always_inline]]
inline void start_measuring_time()
{
    internal::time_measurement_start_iteration();
}

[[gnu::always_inline]]
inline void stop_measuring_time()
{
    internal::time_measurement_stop_iteration();
}


template<typename T>
void do_not_optimize(const T& v)
{
    asm volatile("" : : "r,m" (v));
}

}

// PERF_TEST and PERF_TEST_F support both synchronous and asynchronous functions.
// The former should return `void`, the latter `future<>`.
// PERF_TEST_C executes a coroutine function, if enabled.
// PERF_TEST_CN executes a coroutine function, if enabled, returning the number of inner-loops.
//
// Test cases may perform multiple operations in a single run, this may be desirable
// if the cost of an individual operation is very small. This allows measuring either
// the latency of throughput depending on how the test in written. In such cases,
// the test function shall return either size_t or future<size_t> for synchronous and
// asynchronous cases respectively. The returned value shall be the number of iterations
// done in a single test run.

#define PERF_TEST_F(test_group, test_case) \
    struct test_##test_group##_##test_case : test_group { \
        [[gnu::always_inline]] inline auto run(); \
    }; \
    static ::perf_tests::internal::test_registrar<test_##test_group##_##test_case> \
    test_##test_group##_##test_case##_registrar(#test_group, #test_case); \
    [[gnu::always_inline]] auto test_##test_group##_##test_case::run()

#define PERF_TEST(test_group, test_case) \
    struct test_##test_group##_##test_case { \
        [[gnu::always_inline]] inline auto run(); \
    }; \
    static ::perf_tests::internal::test_registrar<test_##test_group##_##test_case> \
    test_##test_group##_##test_case##_registrar(#test_group, #test_case); \
    [[gnu::always_inline]] auto test_##test_group##_##test_case::run()


#define PERF_TEST_C(test_group, test_case) \
    struct test_##test_group##_##test_case : test_group { \
        inline future<> run(); \
    }; \
    static ::perf_tests::internal::test_registrar<test_##test_group##_##test_case> \
    test_##test_group##_##test_case##_registrar(#test_group, #test_case); \
    future<> test_##test_group##_##test_case::run()

#define PERF_TEST_CN(test_group, test_case) \
    struct test_##test_group##_##test_case : test_group { \
        inline future<size_t> run(); \
    }; \
    static ::perf_tests::internal::test_registrar<test_##test_group##_##test_case> \
    test_##test_group##_##test_case##_registrar(#test_group, #test_case); \
    future<size_t> test_##test_group##_##test_case::run()


#define CONCAT_IMPL(a, b) a##b
#define CONCAT(a, b) CONCAT_IMPL(a, b)

// Installs a pre-run hook.
//
// Register a function to be called before each test run. These are called before
// all runs include the "dry run" used to estimate the number of iterations, if
// that is used.
//
// Multiple hooks may be registered, they will be called in order of registration.
//
// Hooks cannot be deregistered.
#define PERF_PRE_RUN_HOOK(hook) \
    static int CONCAT(hook_registrar_, __COUNTER__) = ::perf_tests::internal::register_pre_run_hook(hook);
