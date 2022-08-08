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

#include <seastar/core/future.hh>
#include <seastar/core/loop.hh>
#include <seastar/testing/linux_perf_event.hh>

#ifdef SEASTAR_COROUTINES_ENABLED
#include <seastar/core/coroutine.hh>
#endif

using namespace seastar;

namespace perf_tests {
namespace internal {

struct config;

using clock_type = std::chrono::steady_clock;

class perf_stats {
public:
    uint64_t allocations = 0;
    uint64_t tasks_executed = 0;
    uint64_t instructions_retired = 0;

private:
    static uint64_t perf_mallocs();
    static uint64_t perf_tasks_processed();

public:
    perf_stats() = default;
    perf_stats(uint64_t allocations_, uint64_t tasks_executed_, uint64_t instructions_retired_ = 0)
        : allocations(allocations_)
        , tasks_executed(tasks_executed_)
        , instructions_retired(instructions_retired_)
    {}
    perf_stats(perf_stats&& o) noexcept
        : allocations(std::exchange(o.allocations, 0))
        , tasks_executed(std::exchange(o.tasks_executed, 0))
        , instructions_retired(std::exchange(o.instructions_retired, 0))
    {}
    perf_stats(const perf_stats& o) = default;

    perf_stats& operator=(perf_stats&& o) = default;
    perf_stats& operator=(const perf_stats& o) = default;

    perf_stats& operator+=(perf_stats b);
    perf_stats& operator-=(perf_stats b);

    static perf_stats snapshot(linux_perf_event* instructions_retired_counter = nullptr);
};

inline
perf_stats
operator+(perf_stats a, perf_stats b) {
    a.allocations += b.allocations;
    a.tasks_executed += b.tasks_executed;
    a.instructions_retired += b.instructions_retired;
    return a;
}

inline
perf_stats
operator-(perf_stats a, perf_stats b) {
    a.allocations -= b.allocations;
    a.tasks_executed -= b.tasks_executed;
    a.instructions_retired -= b.instructions_retired;
    return a;
}

inline perf_stats& perf_stats::operator+=(perf_stats b) {
    allocations += b.allocations;
    tasks_executed += b.tasks_executed;
    instructions_retired += b.instructions_retired;
    return *this;
}

inline perf_stats& perf_stats::operator-=(perf_stats b) {
    allocations -= b.allocations;
    tasks_executed -= b.tasks_executed;
    instructions_retired -= b.instructions_retired;
    return *this;
}

class performance_test {
    std::string _test_case;
    std::string _test_group;

    uint64_t _single_run_iterations = 0;
    std::atomic<uint64_t> _max_single_run_iterations;
protected:
    linux_perf_event _instructions_retired_counter = linux_perf_event::user_instructions_retired();
private:
    void do_run(const config&);
public:
    struct run_result {
        clock_type::duration duration;
        perf_stats stats;
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

// Helper for measuring time.
// Each microbenchmark can either use the default behaviour which measures
// only the start and stop time of the whole run or manually invoke
// start_measuring_time() and stop_measuring_time() in order to measure
// only parts of each iteration.
class time_measurement {
    clock_type::time_point _run_start_time;
    clock_type::time_point _start_time;
    clock_type::duration _total_time;

    perf_stats _start_stats;
    perf_stats _total_stats;

    linux_perf_event* _instructions_retired_counter = nullptr;

public:
    [[gnu::always_inline]] [[gnu::hot]]
    void start_run(linux_perf_event* instructions_retired_counter = nullptr) {
        _instructions_retired_counter = instructions_retired_counter;
        _total_time = { };
        _total_stats = {};
        auto t = clock_type::now();
        _run_start_time = t;
        _start_time = t;
        _start_stats = perf_stats::snapshot(_instructions_retired_counter);
    }

    [[gnu::always_inline]] [[gnu::hot]]
    performance_test::run_result stop_run() {
        auto t = clock_type::now();
        performance_test::run_result ret;
        if (_start_time == _run_start_time) {
            ret.duration = t - _start_time;
            auto stats = perf_stats::snapshot(_instructions_retired_counter);
            ret.stats = stats - _start_stats;
        } else {
            ret.duration = _total_time;
            ret.stats = _total_stats;
        }
        _instructions_retired_counter = nullptr;
        return ret;
    }

    [[gnu::always_inline]] [[gnu::hot]]
    void start_iteration() {
        _start_time = clock_type::now();
        _start_stats = perf_stats::snapshot(_instructions_retired_counter);
    }

    [[gnu::always_inline]] [[gnu::hot]]
    void stop_iteration() {
        auto t = clock_type::now();
        _total_time += t - _start_time;
        perf_stats stats;
        stats = perf_stats::snapshot(_instructions_retired_counter);
        _total_stats += stats - _start_stats;
    }
};

extern time_measurement measure_time;

namespace {

template<bool Condition, typename TrueFn, typename FalseFn>
struct do_if_constexpr_ : FalseFn {
    do_if_constexpr_(TrueFn, FalseFn false_fn) : FalseFn(std::move(false_fn)) { }
    decltype(auto) operator()() const {
        // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=64095
        return FalseFn::operator()(0);
    }
};
template<typename TrueFn, typename FalseFn>
struct do_if_constexpr_<true, TrueFn, FalseFn> : TrueFn {
    do_if_constexpr_(TrueFn true_fn, FalseFn) : TrueFn(std::move(true_fn)) { }
    decltype(auto) operator()() const { return TrueFn::operator()(0); }
};

template<bool Condition, typename TrueFn, typename FalseFn>
do_if_constexpr_<Condition, TrueFn, FalseFn> if_constexpr_(TrueFn&& true_fn, FalseFn&& false_fn)
{
    return do_if_constexpr_<Condition, TrueFn, FalseFn>(std::forward<TrueFn>(true_fn),
                                                        std::forward<FalseFn>(false_fn));
}

}

template<typename Test>
class concrete_performance_test final : public performance_test {
    std::optional<Test> _test;
private:
    template<typename... Args>
    auto run_test(Args&&...) {
        return _test->run();
    }

protected:
    virtual void set_up() override {
        _test.emplace();
    }

    virtual void tear_down() noexcept override {
        _test = std::nullopt;
    }

    [[gnu::hot]]
    virtual future<run_result> do_single_run() override {
        // Redundant 'this->'s courtesy of https://gcc.gnu.org/bugzilla/show_bug.cgi?id=61636
        _instructions_retired_counter.enable();
        return if_constexpr_<is_future<decltype(_test->run())>::value>([&] (auto&&...) {
            measure_time.start_run(&_instructions_retired_counter);
            return do_until([this] { return this->stop_iteration(); }, [this] {
                return if_constexpr_<std::is_same<decltype(_test->run()), future<>>::value>([&] (auto&&...) {
                    this->next_iteration(1);
                    return _test->run();
                }, [&] (auto&&... dependency) {
                    // We need `dependency` to make sure the compiler won't be able to instantiate anything
                    // (and notice that the code does not compile) if this part of if_constexpr_ is not active.
                    return run_test(dependency...).then([&] (size_t n) {
                        this->next_iteration(n);
                    });
                })();
            }).then([] {
                return measure_time.stop_run();
            }).finally([this] {
                _instructions_retired_counter.disable();
            });
        }, [&] (auto&&...) {
            measure_time.start_run(&_instructions_retired_counter);
            while (!stop_iteration()) {
                if_constexpr_<std::is_void<decltype(_test->run())>::value>([&] (auto&&...) {
                    (void)_test->run();
                    this->next_iteration(1);
                }, [&] (auto&&... dependency) {
                    // We need `dependency` to make sure the compiler won't be able to instantiate anything
                    // (and notice that the code does not compile) if this part of if_constexpr_ is not active.
                    this->next_iteration(run_test(dependency...));
                })();
            }
            auto ret = measure_time.stop_run();
            _instructions_retired_counter.disable();
            return make_ready_future<run_result>(std::move(ret));
        })();
    }
public:
    using performance_test::performance_test;
};

void register_test(std::unique_ptr<performance_test>);

template<typename Test>
struct test_registrar {
    test_registrar(const std::string& test_group, const std::string& test_case) {
        auto test = std::make_unique<concrete_performance_test<Test>>(test_case, test_group);
        performance_test::register_test(std::move(test));
    }
};

}

[[gnu::always_inline]]
inline void start_measuring_time()
{
    internal::measure_time.start_iteration();
}

[[gnu::always_inline]]
inline void stop_measuring_time()
{
    internal::measure_time.stop_iteration();
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

#ifdef SEASTAR_COROUTINES_ENABLED

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

#endif // SEASTAR_COROUTINES_ENABLED
