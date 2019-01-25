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
#include <seastar/core/future-util.hh>


using namespace seastar;

namespace perf_tests {
namespace internal {

struct config;

using clock_type = std::chrono::steady_clock;

class performance_test {
    std::string _test_case;
    std::string _test_group;

    uint64_t _single_run_iterations = 0;
    std::atomic<uint64_t> _max_single_run_iterations;
private:
    void do_run(const config&);
protected:
    [[gnu::always_inline]] [[gnu::hot]]
    bool stop_iteration() const {
        return _single_run_iterations >= _max_single_run_iterations.load(std::memory_order_relaxed);
    }

    [[gnu::always_inline]] [[gnu::hot]]
    void next_iteration() {
        _single_run_iterations++;
    }

    virtual void set_up() = 0;
    virtual void tear_down() noexcept = 0;
    virtual future<clock_type::duration> do_single_run() = 0;
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
public:
    [[gnu::always_inline]] [[gnu::hot]]
    void start_run() {
        _total_time = { };
        auto t = clock_type::now();
        _run_start_time = t;
        _start_time = t;
    }

    [[gnu::always_inline]] [[gnu::hot]]
    clock_type::duration stop_run() {
        auto t = clock_type::now();
        if (_start_time == _run_start_time) {
            return t - _start_time;
        }
        return _total_time;
    }

    [[gnu::always_inline]] [[gnu::hot]]
    void start_iteration() {
        _start_time = clock_type::now();
    }
    
    [[gnu::always_inline]] [[gnu::hot]]
    void stop_iteration() {
        auto t = clock_type::now();
        _total_time += t - _start_time;
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
    compat::optional<Test> _test;
protected:
    virtual void set_up() override {
        _test.emplace();
    }

    virtual void tear_down() noexcept override {
        _test = compat::nullopt;
    }

    [[gnu::hot]]
    virtual future<clock_type::duration> do_single_run() override {
        // Redundant 'this->'s courtesy of https://gcc.gnu.org/bugzilla/show_bug.cgi?id=61636
        return if_constexpr_<is_future<decltype(_test->run())>::value>([&] (auto&&...) {
            measure_time.start_run();
            return do_until([this] { return this->stop_iteration(); }, [this] {
                this->next_iteration();
                return _test->run();
            }).then([] {
                return measure_time.stop_run();
            });
        }, [&] (auto&&...) {
            measure_time.start_run();
            while (!stop_iteration()) {
                this->next_iteration();
                _test->run();
            }
            return make_ready_future<clock_type::duration>(measure_time.stop_run());
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
