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
 * Copyright (C) 2019 ScyllaDB Ltd.
 */

#include <exception>
#include <numeric>
#include <ranges>

#include <seastar/core/circular_buffer.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/thread.hh>
#include <seastar/coroutine/all.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/coroutine/switch_to.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/coroutine/generator.hh>
#include <seastar/testing/random.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/util/later.hh>

using seastar::broken_promise;
using seastar::circular_buffer;
using seastar::create_scheduling_group;
using seastar::current_scheduling_group;
using seastar::default_scheduling_group;
using seastar::future;
using seastar::make_exception_future;
using seastar::make_ready_future;
using seastar::need_preempt;
using seastar::promise;
using seastar::scheduling_group;
using seastar::semaphore;
using seastar::semaphore_timed_out;
using seastar::sleep;
using seastar::yield;

namespace coroutine = seastar::coroutine;
namespace testing = seastar::testing;

using namespace std::chrono_literals;

namespace {

future<int> old_fashioned_continuations() {
    return yield().then([] {
        return 42;
    });
}

future<int> simple_coroutine() {
    co_await yield();
    co_return 53;
}

future<int> ready_coroutine() {
    co_return 64;
}

future<std::tuple<int, double>> tuple_coroutine() {
    co_return std::tuple(1, 2.);
}

future<int> failing_coroutine() {
    co_await yield();
    throw 42;
}

[[gnu::noinline]] int throw_exception(int x) {
    throw x;
}

future<int> failing_coroutine2() noexcept {
    co_await yield();
    co_return throw_exception(17);
}

}

SEASTAR_TEST_CASE(test_simple_coroutines) {
    BOOST_REQUIRE_EQUAL(co_await old_fashioned_continuations(), 42);
    BOOST_REQUIRE_EQUAL(co_await simple_coroutine(), 53);
    BOOST_REQUIRE_EQUAL(ready_coroutine().get(), 64);
    BOOST_REQUIRE(co_await tuple_coroutine() == std::tuple(1, 2.));
    BOOST_REQUIRE_EXCEPTION((void)co_await failing_coroutine(), int, [] (auto v) { return v == 42; });
    BOOST_CHECK_EQUAL(co_await failing_coroutine().then_wrapped([] (future<int> f) -> future<int> {
        BOOST_REQUIRE(f.failed());
        try {
            std::rethrow_exception(f.get_exception());
        } catch (int v) {
           co_return v;
        }
    }), 42);
    BOOST_REQUIRE_EXCEPTION((void)co_await failing_coroutine2(), int, [] (auto v) { return v == 17; });
    BOOST_CHECK_EQUAL(co_await failing_coroutine2().then_wrapped([] (future<int> f) -> future<int> {
        BOOST_REQUIRE(f.failed());
        try {
            std::rethrow_exception(f.get_exception());
        } catch (int v) {
           co_return v;
        }
    }), 17);
}

SEASTAR_TEST_CASE(test_abandond_coroutine) {
    std::optional<future<int>> f;
    {
        auto p1 = promise<>();
        auto p2 = promise<>();
        auto p3 = promise<>();
        f = p1.get_future().then([&] () -> future<int> {
            p2.set_value();
            BOOST_CHECK_THROW(co_await p3.get_future(), broken_promise);
            co_return 1;
        });
        p1.set_value();
        co_await p2.get_future();
    }
    BOOST_CHECK_EQUAL(co_await std::move(*f), 1);
}

SEASTAR_TEST_CASE(test_scheduling_group) {
    auto other_sg = co_await create_scheduling_group("the other group", 10.f);
    std::exception_ptr ex;

    try {
        auto p1 = promise<>();
        auto p2 = promise<>();

        auto p1b = promise<>();
        auto p2b = promise<>();
        auto f1 = p1b.get_future();
        auto f2 = p2b.get_future();

        BOOST_REQUIRE(current_scheduling_group() == default_scheduling_group());
        auto f_ret = with_scheduling_group(other_sg,
                [other_sg_cap = other_sg] (future<> f1, future<> f2, promise<> p1, promise<> p2) -> future<int> {
            // Make a copy in the coroutine before the lambda is destroyed.
            auto other_sg = other_sg_cap;
            BOOST_REQUIRE(current_scheduling_group() == other_sg);
            BOOST_REQUIRE(other_sg == other_sg);
            p1.set_value();
            co_await std::move(f1);
            BOOST_REQUIRE(current_scheduling_group() == other_sg);
            p2.set_value();
            co_await std::move(f2);
            BOOST_REQUIRE(current_scheduling_group() == other_sg);
            co_return 42;
        }, p1.get_future(), p2.get_future(), std::move(p1b), std::move(p2b));

        co_await std::move(f1);
        BOOST_REQUIRE(current_scheduling_group() == default_scheduling_group());
        p1.set_value();
        co_await std::move(f2);
        BOOST_REQUIRE(current_scheduling_group() == default_scheduling_group());
        p2.set_value();
        BOOST_REQUIRE_EQUAL(co_await std::move(f_ret), 42);
        BOOST_REQUIRE(current_scheduling_group() == default_scheduling_group());
    } catch (...) {
        ex = std::current_exception();
    }
    co_await destroy_scheduling_group(other_sg);
    if (ex) {
        std::rethrow_exception(std::move(ex));
    }
}

future<scheduling_group> switch_to_with_context(scheduling_group& sg) {
    scheduling_group new_sg = co_await coroutine::switch_to(sg);
    BOOST_REQUIRE(current_scheduling_group() == sg);
    co_return new_sg;
}

SEASTAR_TEST_CASE(test_switch_to) {
    auto other_sg0 = co_await create_scheduling_group("other group 0", 10.f);
    auto other_sg1 = co_await create_scheduling_group("other group 1", 10.f);
    auto other_sg2 = co_await create_scheduling_group("other group 2", 10.f);
    std::exception_ptr ex;

    try {
        auto base_sg = current_scheduling_group();

        auto prev_sg = co_await coroutine::switch_to(other_sg0);
        BOOST_REQUIRE(current_scheduling_group() == other_sg0);
        BOOST_REQUIRE(prev_sg == base_sg);

        auto same_sg = co_await coroutine::switch_to(other_sg0);
        BOOST_REQUIRE(current_scheduling_group() == other_sg0);
        BOOST_REQUIRE(same_sg == other_sg0);

        auto nested_sg = co_await coroutine::switch_to(other_sg1);
        BOOST_REQUIRE(current_scheduling_group() == other_sg1);
        BOOST_REQUIRE(nested_sg == other_sg0);

        co_await switch_to_with_context(other_sg2);

        co_await coroutine::switch_to(base_sg);
        BOOST_REQUIRE(current_scheduling_group() == base_sg);
    } catch (...) {
        ex = std::current_exception();
    }

    co_await destroy_scheduling_group(other_sg1);
    co_await destroy_scheduling_group(other_sg0);
    if (ex) {
        std::rethrow_exception(std::move(ex));
    }
}

future<> check_thread_inherits_sg_from_coroutine_frame(scheduling_group expected_sg) {
    return seastar::async([expected_sg] {
        BOOST_REQUIRE(current_scheduling_group() == expected_sg);
    });
}

future<> check_coroutine_inherits_sg_from_another_one(scheduling_group expected_sg) {
    co_await yield();
    BOOST_REQUIRE(current_scheduling_group() == expected_sg);
}

future<> switch_to_sg_and_perform_inheriting_checks(scheduling_group base_sg, scheduling_group new_sg) {
    BOOST_REQUIRE(current_scheduling_group() == base_sg);
    co_await coroutine::switch_to(new_sg);
    BOOST_REQUIRE(current_scheduling_group() == new_sg);

    co_await check_thread_inherits_sg_from_coroutine_frame(new_sg);
    co_await check_coroutine_inherits_sg_from_another_one(new_sg);

    // don't restore previous sg on purpose, expecting it will be restored once coroutine goes out of scope
}

SEASTAR_TEST_CASE(test_switch_to_sg_restoration_and_inheriting) {
    auto new_sg = co_await create_scheduling_group("other group 0", 10.f);
    std::exception_ptr ex;

    try {
        auto base_sg = current_scheduling_group();

        co_await switch_to_sg_and_perform_inheriting_checks(base_sg, new_sg);
        // seastar automatically restores base_sg once it goes out of coroutine frame
        BOOST_REQUIRE(current_scheduling_group() == base_sg);

        co_await seastar::async([base_sg, new_sg] {
            switch_to_sg_and_perform_inheriting_checks(base_sg, new_sg).get();
            BOOST_REQUIRE(current_scheduling_group() == base_sg);
        });

        co_await switch_to_sg_and_perform_inheriting_checks(base_sg, new_sg).finally([base_sg] {
            BOOST_REQUIRE(current_scheduling_group() == base_sg);
        });
    } catch (...) {
        ex = std::current_exception();
    }

    co_await destroy_scheduling_group(new_sg);
    if (ex) {
        std::rethrow_exception(std::move(ex));
    }
}

SEASTAR_TEST_CASE(test_preemption) {
    bool x = false;
    unsigned preempted = 0;
    auto f = yield().then([&x] {
            x = true;
        });

    // try to preempt 1000 times. 1 should be enough if not for
    // task queue shaffling in debug mode which may cause co-routine
    // continuation to run first.
    while(preempted < 1000 && !x) {
        preempted += need_preempt();
        co_await make_ready_future<>();
    }
    auto save_x = x;
    // wait for yield() to complete
    co_await std::move(f);
    BOOST_REQUIRE(save_x);
    co_return;
}

SEASTAR_TEST_CASE(test_no_preemption) {
    bool x = false;
    unsigned preempted = 0;
    auto f = yield().then([&x] {
            x = true;
        });

    // preemption should not happen, we explicitly asked for continuing if possible
    while(preempted < 1000 && !x) {
        preempted += need_preempt();
        co_await coroutine::without_preemption_check(make_ready_future<>());
    }
    auto save_x = x;
    // wait for yield() to complete
    co_await std::move(f);
    BOOST_REQUIRE(!save_x);
    co_return;
}

SEASTAR_TEST_CASE(test_all_simple) {
    auto [a, b] = co_await coroutine::all(
        [] { return make_ready_future<int>(1); },
        [] { return make_ready_future<int>(2); }
    );
    BOOST_REQUIRE_EQUAL(a, 1);
    BOOST_REQUIRE_EQUAL(b, 2);
}

SEASTAR_TEST_CASE(test_all_permutations) {
    std::vector<std::chrono::milliseconds> delays = { 0ms, 0ms, 2ms, 2ms, 4ms, 6ms };
    auto make_delayed_future_returning_nr = [&] (int nr) {
        return [=] {
            auto delay = delays[nr];
            return delay == 0ms ? make_ready_future<int>(nr) : sleep(delay).then([nr] { return make_ready_future<int>(nr); });
        };
    };
    do {
        auto [a, b, c, d, e, f] = co_await coroutine::all(
            make_delayed_future_returning_nr(0),
            make_delayed_future_returning_nr(1),
            make_delayed_future_returning_nr(2),
            make_delayed_future_returning_nr(3),
            make_delayed_future_returning_nr(4),
            make_delayed_future_returning_nr(5)
        );
        BOOST_REQUIRE_EQUAL(a, 0);
        BOOST_REQUIRE_EQUAL(b, 1);
        BOOST_REQUIRE_EQUAL(c, 2);
        BOOST_REQUIRE_EQUAL(d, 3);
        BOOST_REQUIRE_EQUAL(e, 4);
        BOOST_REQUIRE_EQUAL(f, 5);
    } while (std::ranges::next_permutation(delays).found);
}

SEASTAR_TEST_CASE(test_all_ready_exceptions) {
    try {
        co_await coroutine::all(
            [] () -> future<> { throw 1; },
            [] () -> future<> { throw 2; }
        );
    } catch (int e) {
        BOOST_REQUIRE(e == 1 || e == 2);
    }
}

SEASTAR_TEST_CASE(test_all_nonready_exceptions) {
    try {
        co_await coroutine::all(
            [] () -> future<> {
                co_await sleep(1ms);
                throw 1;
            },
            [] () -> future<> {
                co_await sleep(1ms);
                throw 2;
            }
        );
    } catch (int e) {
        BOOST_REQUIRE(e == 1 || e == 2);
    }
}

SEASTAR_TEST_CASE(test_all_heterogeneous_types) {
    auto [a, b] = co_await coroutine::all(
        [] () -> future<int> {
            co_await sleep(1ms);
            co_return 1;
        },
        [] () -> future<> {
            co_await sleep(1ms);
        },
        [] () -> future<long> {
            co_await sleep(1ms);
            co_return 2L;
        }
    );
    BOOST_REQUIRE_EQUAL(a, 1);
    BOOST_REQUIRE_EQUAL(b, 2L);
}

SEASTAR_TEST_CASE(test_all_noncopyable_types) {
    auto [a] = co_await coroutine::all(
        [] () -> future<std::unique_ptr<int>> {
            co_return std::make_unique<int>(6);
        }
    );
    BOOST_REQUIRE_EQUAL(*a, 6);
}

SEASTAR_TEST_CASE(test_all_throw_in_input_func) {
    int nr_completed = 0;
    bool exception_seen = false;
    try {
        co_await coroutine::all(
            [&] () -> future<int> {
                co_await sleep(1ms);
                ++nr_completed;
                co_return 7;
            },
            [&] () -> future<int> {
                throw 9;
            },
            [&] () -> future<int> {
                co_await sleep(1ms);
                ++nr_completed;
                co_return 7;
            }
        );
    } catch (int n) {
        BOOST_REQUIRE_EQUAL(n, 9);
        exception_seen = true;
    }
    BOOST_REQUIRE_EQUAL(nr_completed, 2);
    BOOST_REQUIRE(exception_seen);
}

struct counter_ref {
private:
    int& _counter;

public:
    explicit counter_ref(int& cnt)
            : _counter(cnt) {
        ++_counter;
    }

    ~counter_ref() {
        --_counter;
    }
};

template<typename Ex>
static future<> check_coroutine_throws(auto fun) {
    // The counter keeps track of the number of alive "counter_ref" objects.
    // If it is not zero then it means that some destructors weren't run
    // while quitting the coroutine.
    int counter = 0;
    BOOST_REQUIRE_THROW(co_await fun(counter), Ex);
    BOOST_REQUIRE_EQUAL(counter, 0);
    co_await fun(counter).then_wrapped([&counter] (auto f) {
        BOOST_REQUIRE(f.failed());
        BOOST_REQUIRE_THROW(std::rethrow_exception(f.get_exception()), Ex);
        BOOST_REQUIRE_EQUAL(counter, 0);
    });
}

SEASTAR_TEST_CASE(test_coroutine_exception) {
    co_await check_coroutine_throws<std::runtime_error>([] (int& counter) -> future<int> {
        counter_ref ref{counter};
        co_return coroutine::exception(std::make_exception_ptr(std::runtime_error("threw")));
    });
    co_await check_coroutine_throws<std::runtime_error>([] (int& counter) -> future<int> {
        counter_ref ref{counter};
        co_await coroutine::exception(std::make_exception_ptr(std::runtime_error("threw")));
        co_return 42;
    });
    co_await check_coroutine_throws<std::logic_error>([] (int& counter) -> future<> {
        counter_ref ref{counter};
        co_await coroutine::return_exception(std::logic_error("threw"));
        co_return;
    });
    co_await check_coroutine_throws<int>([] (int& counter) -> future<> {
        counter_ref ref{counter};
        co_await coroutine::return_exception(42);
        co_return;
    });
}

SEASTAR_TEST_CASE(test_coroutine_return_exception_ptr) {
    co_await check_coroutine_throws<std::runtime_error>([] (int& counter) -> future<> {
        co_await coroutine::return_exception(std::runtime_error("threw"));
    });
    co_await check_coroutine_throws<std::runtime_error>([] (int& counter) -> future<> {
        auto ex = std::make_exception_ptr(std::runtime_error("threw"));
        co_await coroutine::return_exception_ptr(std::move(ex));
    });
    co_await check_coroutine_throws<std::runtime_error>([] (int& counter) -> future<> {
        auto ex = std::make_exception_ptr(std::runtime_error("threw"));
        co_await coroutine::return_exception_ptr(ex);
    });
    co_await check_coroutine_throws<int>([] (int& counter) -> future<> {
        co_await coroutine::return_exception_ptr(std::make_exception_ptr(3));
    });
}

SEASTAR_TEST_CASE(test_maybe_yield) {
    int var = 0;
    bool done = false;
    auto spinner = [&] () -> future<> {
        // increment a variable continuously, but yield so an observer can see it.
        while (!done) {
            ++var;
            co_await coroutine::maybe_yield();
        }
    };
    auto spinner_fut = spinner();
    int snapshot = var;
    for (int nr_changes = 0; nr_changes < 10; ++nr_changes) {
        // Try to observe the value changing in time, yield to
        // allow the spinner to advance it.
        while (snapshot == var) {
            co_await coroutine::maybe_yield();
        }
        snapshot = var;
    }
    done = true;
    co_await std::move(spinner_fut);
    BOOST_REQUIRE(true); // the test will hang if it doesn't work.
}

#ifndef __clang__

#include "tl-generator.hh"
tl::generator<int> simple_generator(int max)
{
    for (int i = 0; i < max; ++i) {
        co_yield i;
    }
}

SEASTAR_TEST_CASE(generator)
{
    // test ability of seastar::parallel_for_each to deal with move-only views
    int accum = 0;
    co_await seastar::parallel_for_each(simple_generator(10), [&](int i) {
        accum += i;
        return seastar::make_ready_future<>();
    });
    BOOST_REQUIRE_EQUAL(accum, 45);

    // test ability of seastar::max_concurrent_for_each to deal with move-only views
    accum = 0;
    co_await seastar::max_concurrent_for_each(simple_generator(10), 10, [&](int i) {
        accum += i;
        return seastar::make_ready_future<>();
    });
    BOOST_REQUIRE_EQUAL(accum, 45);
}

#endif

SEASTAR_TEST_CASE(test_parallel_for_each_empty) {
    std::vector<int> values;
    int count = 0;

    co_await coroutine::parallel_for_each(values, [&] (int x) {
        ++count;
    });
    BOOST_REQUIRE_EQUAL(count, 0); // the test will hang if it doesn't work.
}

SEASTAR_TEST_CASE(test_parallel_for_each_exception) {
    std::array<int, 5> values = { 10, 2, 1, 4, 8 };
    int count = 0;
    auto& eng = testing::local_random_engine;
    auto dist = std::uniform_int_distribution<unsigned>();
    int throw_at = dist(eng) % values.size();

    BOOST_TEST_MESSAGE(fmt::format("Will throw at value #{}/{}", throw_at, values.size()));

    auto f0 = coroutine::parallel_for_each(values, [&] (int x) {
        if (count++ == throw_at) {
            BOOST_TEST_MESSAGE("throw");
            throw std::runtime_error("test");
        }
    });
    // An exception thrown by the functor must be propagated
    BOOST_REQUIRE_THROW(co_await std::move(f0), std::runtime_error);
    // Functor must be called on all values, even if there's an exception
    BOOST_REQUIRE_EQUAL(count, values.size());

    count = 0;
    throw_at = dist(eng) % values.size();
    BOOST_TEST_MESSAGE(fmt::format("Will throw at value #{}/{}", throw_at, values.size()));

    auto f1 = coroutine::parallel_for_each(values, [&] (int x) -> future<> {
        co_await sleep(std::chrono::milliseconds(x));
        if (count++ == throw_at) {
            throw std::runtime_error("test");
        }
    });
    BOOST_REQUIRE_THROW(co_await std::move(f1), std::runtime_error);
    BOOST_REQUIRE_EQUAL(count, values.size());
}

SEASTAR_TEST_CASE(test_parallel_for_each) {
    std::vector<int> values = { 3, 1, 4 };
    int sum_of_squares = 0;

    int expected = std::accumulate(values.begin(), values.end(), 0, [] (int sum, int x) {
        return sum + x * x;
    });

    // Test all-ready futures
    co_await coroutine::parallel_for_each(values, [&sum_of_squares] (int x) {
        sum_of_squares += x * x;
    });
    BOOST_REQUIRE_EQUAL(sum_of_squares, expected);

    // Test non-ready futures
    sum_of_squares = 0;
    co_await coroutine::parallel_for_each(values, [&sum_of_squares] (int x) -> future<> {
        if (x > 1) {
            co_await sleep(std::chrono::milliseconds(x));
        }
        sum_of_squares += x * x;
    });
    BOOST_REQUIRE_EQUAL(sum_of_squares, expected);

    // Test legacy subrange
    sum_of_squares = 0;
    co_await coroutine::parallel_for_each(values.begin(), values.end() - 1, [&sum_of_squares] (int x) -> future<> {
        if (x > 1) {
            co_await sleep(std::chrono::milliseconds(x));
        }
        sum_of_squares += x * x;
    });
    BOOST_REQUIRE_EQUAL(sum_of_squares, 10);

    // clang 13.0.1 doesn't support subrange
    // so provide also a Iterator/Sentinel based constructor.
    // See https://github.com/llvm/llvm-project/issues/46091
#ifndef __clang__
    // Test std::ranges::subrange
    sum_of_squares = 0;
    co_await coroutine::parallel_for_each(std::ranges::subrange(values.begin(), values.end() - 1), [&sum_of_squares] (int x) -> future<> {
        if (x > 1) {
            co_await sleep(std::chrono::milliseconds(x));
        }
        sum_of_squares += x * x;
    });
    BOOST_REQUIRE_EQUAL(sum_of_squares, 10);
#endif
}

SEASTAR_TEST_CASE(test_void_as_future) {
    auto f = co_await coroutine::as_future(make_ready_future<>());
    BOOST_REQUIRE(f.available());

    f = co_await coroutine::as_future(make_exception_future<>(std::runtime_error("exception")));
    BOOST_REQUIRE_THROW(f.get(), std::runtime_error);

    semaphore sem(0);
    (void)sleep(1ms).then([&] { sem.signal(); });
    f = co_await coroutine::as_future(sem.wait());
    BOOST_REQUIRE(f.available());

    f = co_await coroutine::as_future(sem.wait(duration_cast<semaphore::duration>(1ms)));
    BOOST_REQUIRE_THROW(f.get(), semaphore_timed_out);
}

SEASTAR_TEST_CASE(test_void_as_future_without_preemption_check) {
    auto f = co_await coroutine::as_future_without_preemption_check(make_ready_future<>());
    BOOST_REQUIRE(f.available());

    f = co_await coroutine::as_future_without_preemption_check(make_exception_future<>(std::runtime_error("exception")));
    BOOST_REQUIRE_THROW(f.get(), std::runtime_error);

    semaphore sem(0);
    (void)sleep(1ms).then([&] { sem.signal(); });
    f = co_await coroutine::as_future_without_preemption_check(sem.wait());
    BOOST_REQUIRE(f.available());

    f = co_await coroutine::as_future_without_preemption_check(sem.wait(duration_cast<semaphore::duration>(1ms)));
    BOOST_REQUIRE_THROW(f.get(), semaphore_timed_out);
}

SEASTAR_TEST_CASE(test_non_void_as_future) {
    auto f = co_await coroutine::as_future(make_ready_future<int>(42));
    BOOST_REQUIRE_EQUAL(f.get(), 42);

    f = co_await coroutine::as_future(make_exception_future<int>(std::runtime_error("exception")));
    BOOST_REQUIRE_THROW(f.get(), std::runtime_error);

    auto p = promise<int>();
    (void)sleep(1ms).then([&] { p.set_value(314); });
    f = co_await coroutine::as_future(p.get_future());
    BOOST_REQUIRE_EQUAL(f.get(), 314);

    auto gen_exception = [] () -> future<int> {
        co_await sleep(1ms);
        co_return coroutine::exception(std::make_exception_ptr(std::runtime_error("exception")));
    };
    f = co_await coroutine::as_future(gen_exception());
    BOOST_REQUIRE_THROW(f.get(), std::runtime_error);
}

SEASTAR_TEST_CASE(test_non_void_as_future_without_preemption_check) {
    auto f = co_await coroutine::as_future_without_preemption_check(make_ready_future<int>(42));
    BOOST_REQUIRE_EQUAL(f.get(), 42);

    f = co_await coroutine::as_future_without_preemption_check(make_exception_future<int>(std::runtime_error("exception")));
    BOOST_REQUIRE_THROW(f.get(), std::runtime_error);

    auto p = promise<int>();
    (void)sleep(1ms).then([&] { p.set_value(314); });
    f = co_await coroutine::as_future_without_preemption_check(p.get_future());
    BOOST_REQUIRE_EQUAL(f.get(), 314);

    auto gen_exception = [] () -> future<int> {
        co_await sleep(1ms);
        co_return coroutine::exception(std::make_exception_ptr(std::runtime_error("exception")));
    };
    f = co_await coroutine::as_future_without_preemption_check(gen_exception());
    BOOST_REQUIRE_THROW(f.get(), std::runtime_error);
}

SEASTAR_TEST_CASE(test_as_future_preemption) {
    bool stop = false;

    auto get_ready_future = [&] {
        return stop ? make_exception_future<>(std::runtime_error("exception")) : make_ready_future<>();
    };

    auto wait_for_stop = [&] () -> future<bool> {
        for (;;) {
            auto f = co_await coroutine::as_future(get_ready_future());
            if (f.failed()) {
                co_return coroutine::exception(f.get_exception());
            }
        }
    };

    auto f0 = wait_for_stop();

    auto set_stop = [&] () -> future<> {
        for (;;) {
            stop = true;
            if (f0.available()) {
                co_return;
            }
            co_await coroutine::maybe_yield();
        }
    };

    co_await set_stop();

    BOOST_REQUIRE_THROW(f0.get(), std::runtime_error);
}

template<template<typename> class Container>
coroutine::experimental::generator<int, Container>
fibonacci_sequence(coroutine::experimental::buffer_size_t size, unsigned count) {
    auto a = 0, b = 1;
    for (unsigned i = 0; i < count; ++i) {
        if (std::numeric_limits<decltype(a)>::max() - a < b) {
            throw std::out_of_range(
                fmt::format("fibonacci[{}] is greater than the largest value of int", i));
        }
        co_yield std::exchange(a, std::exchange(b, a + b));
    }
}

template<template<typename> class Container>
seastar::future<> test_async_generator_drained() {
    auto expected_fibs = {0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55};
    auto fib = fibonacci_sequence<Container>(coroutine::experimental::buffer_size_t{2},
                                             std::size(expected_fibs));
    for (auto expected_fib : expected_fibs) {
        auto actual_fib = co_await fib();
        BOOST_REQUIRE(actual_fib.has_value());
        BOOST_REQUIRE_EQUAL(actual_fib.value(), expected_fib);
    }
    auto sentinel = co_await fib();
    BOOST_REQUIRE(!sentinel.has_value());
}

template<typename T>
using buffered_container = circular_buffer<T>;

SEASTAR_TEST_CASE(test_async_generator_drained_buffered) {
    return test_async_generator_drained<buffered_container>();
}

SEASTAR_TEST_CASE(test_async_generator_drained_unbuffered) {
    return test_async_generator_drained<std::optional>();
}

template<template<typename> class Container>
seastar::future<> test_async_generator_not_drained() {
    auto fib = fibonacci_sequence<Container>(coroutine::experimental::buffer_size_t{2},
                                             42);
    auto actual_fib = co_await fib();
    BOOST_REQUIRE(actual_fib.has_value());
    BOOST_REQUIRE_EQUAL(actual_fib.value(), 0);
}

SEASTAR_TEST_CASE(test_async_generator_not_drained_buffered) {
    return test_async_generator_not_drained<buffered_container>();
}

SEASTAR_TEST_CASE(test_async_generator_not_drained_unbuffered) {
    return test_async_generator_not_drained<std::optional>();
}

struct counter_t {
    int n;
    int* count;
    counter_t(counter_t&& other) noexcept
        : n{std::exchange(other.n, -1)},
          count{std::exchange(other.count, nullptr)}
    {}
    counter_t(int n, int* count) noexcept
        : n{n}, count{count} {
        ++(*count);
    }
    ~counter_t() noexcept {
        if (count) {
            --(*count);
        }
    }
};
std::ostream& operator<<(std::ostream& os, const counter_t& c) {
    return os << c.n;
}

template<template<typename> class Container>
coroutine::experimental::generator<counter_t, Container>
fiddle(coroutine::experimental::buffer_size_t size, int n, int* total) {
    int i = 0;
    while (true) {
        if (i++ == n) {
            throw std::invalid_argument("Eureka from generator!");
        }
        co_yield counter_t{i, total};
    }
}

template<template<typename> class Container>
seastar::future<> test_async_generator_throws_from_generator() {
    int total = 0;
    auto count_to = [total=&total](unsigned n) -> seastar::future<> {
        auto count = fiddle<Container>(coroutine::experimental::buffer_size_t{2},
                                       n, total);
        for (unsigned i = 0; i < 2 * n; i++) {
            co_await count();
        }
    };
    co_await count_to(42).then_wrapped([&total] (auto f) {
        BOOST_REQUIRE(f.failed());
        BOOST_REQUIRE_THROW(std::rethrow_exception(f.get_exception()), std::invalid_argument);
        BOOST_REQUIRE_EQUAL(total, 0);
    });
}

SEASTAR_TEST_CASE(test_async_generator_throws_from_generator_buffered) {
    return test_async_generator_throws_from_generator<buffered_container>();
}

SEASTAR_TEST_CASE(test_async_generator_throws_from_generator_unbuffered) {
    return test_async_generator_throws_from_generator<std::optional>();
}

template<template<typename> class Container>
seastar::future<> test_async_generator_throws_from_consumer() {
    int total = 0;
    auto count_to = [total=&total](unsigned n) -> seastar::future<> {
        auto count = fiddle<Container>(coroutine::experimental::buffer_size_t{2},
                                       n, total);
        for (unsigned i = 0; i < n; i++) {
            if (i == n / 2) {
                throw std::invalid_argument("Eureka from consumer!");
            }
            co_await count();
        }
    };
    co_await count_to(42).then_wrapped([&total] (auto f) {
        BOOST_REQUIRE(f.failed());
        BOOST_REQUIRE_THROW(std::rethrow_exception(f.get_exception()), std::invalid_argument);
        BOOST_REQUIRE_EQUAL(total, 0);
    });
}

SEASTAR_TEST_CASE(test_async_generator_throws_from_consumer_buffered) {
    return test_async_generator_throws_from_consumer<buffered_container>();
}

SEASTAR_TEST_CASE(test_async_generator_throws_from_consumer_unbuffered) {
    return test_async_generator_throws_from_consumer<std::optional>();
}

SEASTAR_TEST_CASE(test_lambda_coroutine_in_continuation) {
    auto dist = std::uniform_real_distribution<>(0.0, 1.0);
    auto rand_eng = std::default_random_engine(std::random_device()());
    double n = dist(rand_eng);
    auto sin1 = std::sin(n); // avoid optimizer tricks
    auto boo = std::array<char, 1025>(); // bias coroutine size towards 1024 size class
    auto sin2 = co_await yield().then(coroutine::lambda([n, boo] () -> future<double> {
        // Expect coroutine capture to be freed after co_await without coroutine::lambda
        co_await yield();
        // Try to overwrite recently-release coroutine frame by allocating in similar size-class
        std::vector<char*> garbage;
        for (size_t sz = 1024; sz < 2048; ++sz) {
            for (int ctr = 0; ctr < 100; ++ctr) {
                auto p = static_cast<char*>(malloc(sz));
                std::memset(p, 0, sz);
                garbage.push_back(p);
            }
        }
        for (auto p : garbage) {
            std::free(p);
        }
        (void)boo;
        co_return std::sin(n);
    }));
    BOOST_REQUIRE_EQUAL(sin1, sin2);
}
