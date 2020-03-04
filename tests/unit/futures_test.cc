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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include <seastar/testing/test_case.hh>

#include <seastar/core/reactor.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/stream.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/manual_clock.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/print.hh>
#include <seastar/util/log.hh>
#include <boost/iterator/counting_iterator.hpp>
#include <seastar/testing/thread_test_case.hh>

#include <boost/range/iterator_range.hpp>
#include <boost/range/irange.hpp>

using namespace seastar;
using namespace std::chrono_literals;

class expected_exception : public std::runtime_error {
public:
    expected_exception() : runtime_error("expected") {}
};

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#endif
SEASTAR_TEST_CASE(test_self_move) {
    future_state<std::unique_ptr<int>> s1;
    s1.set(std::make_unique<int>(42));
    s1 = std::move(s1); // no crash, but the value of s1 is not defined.

    future_state<std::unique_ptr<int>> s2;
    s2.set(std::make_unique<int>(42));
    std::swap(s2, s2);
    BOOST_REQUIRE_EQUAL(*std::get<0>(std::move(s2).get()), 42);

    promise<std::unique_ptr<int>> p1;
    p1.set_value(std::make_unique<int>(42));
    p1 = std::move(p1); // no crash, but the value of p1 is not defined.

    promise<std::unique_ptr<int>> p2;
    p2.set_value(std::make_unique<int>(42));
    std::swap(p2, p2);
    BOOST_REQUIRE_EQUAL(*p2.get_future().get0(), 42);

    auto  f1 = make_ready_future<std::unique_ptr<int>>(std::make_unique<int>(42));
    f1 = std::move(f1); // no crash, but the value of f1 is not defined.

    auto f2 = make_ready_future<std::unique_ptr<int>>(std::make_unique<int>(42));
    std::swap(f2, f2);
    BOOST_REQUIRE_EQUAL(*f2.get0(), 42);

    return make_ready_future<>();
}
#ifdef __clang__
#pragma clang diagnostic pop
#endif

static subscription<int> get_empty_subscription(std::function<future<> (int)> func) {
    stream<int> s;
    auto ret = s.listen(func);
    s.close();
    return ret;
}

SEASTAR_TEST_CASE(test_stream) {
    auto sub = get_empty_subscription([](int x) {
        return make_ready_future<>();
    });
    return sub.done();
}

SEASTAR_TEST_CASE(test_stream_drop_sub) {
    auto s = make_lw_shared<stream<int>>();
    compat::optional<future<>> ret;
    {
        auto sub = s->listen([](int x) {
            return make_ready_future<>();
        });
        ret = sub.done();
        // It is ok to drop the subscription when we only want the competition future.
    }
    return s->produce(42).then([ret = std::move(*ret), s] () mutable {
        s->close();
        return std::move(ret);
    });
}

SEASTAR_TEST_CASE(test_set_future_state_with_tuple) {
    future_state<int> s1;
    promise<int> p1;
    const std::tuple<int> v1(42);
    s1.set(v1);
    p1.set_value(v1);

    future_state<int, int> s2;
    promise<int, int> p2;
    const std::tuple<int, int> v2(41, 42);
    s2.set(v2);
    p2.set_value(v2);

    return make_ready_future<>();
}

SEASTAR_THREAD_TEST_CASE(test_set_value_make_exception_in_copy) {
    struct throw_in_copy {
        throw_in_copy() noexcept = default;
        throw_in_copy(throw_in_copy&& x) noexcept {
        }
        throw_in_copy(const throw_in_copy& x) {
            throw 42;
        }
    };
    promise<throw_in_copy> p1;
    throw_in_copy v;
    p1.set_value(v);
    BOOST_REQUIRE_THROW(p1.get_future().get(), int);
}

SEASTAR_THREAD_TEST_CASE(test_set_exception_in_constructor) {
    struct throw_in_constructor {
        throw_in_constructor() {
            throw 42;
        }
    };
    future<throw_in_constructor> f = make_ready_future<throw_in_constructor>();
    BOOST_REQUIRE(f.failed());
    BOOST_REQUIRE_THROW(f.get(), int);
}

SEASTAR_TEST_CASE(test_finally_is_called_on_success_and_failure) {
    auto finally1 = make_shared<bool>();
    auto finally2 = make_shared<bool>();

    return make_ready_future().then([] {
    }).finally([=] {
        *finally1 = true;
    }).then([] {
        throw std::runtime_error("");
    }).finally([=] {
        *finally2 = true;
    }).then_wrapped([=] (auto&& f) {
        BOOST_REQUIRE(*finally1);
        BOOST_REQUIRE(*finally2);

        // Should be failed.
        try {
            f.get();
            BOOST_REQUIRE(false);
        } catch (...) {}
    });
}

SEASTAR_TEST_CASE(test_get_on_promise) {
    auto p = promise<uint32_t>();
    p.set_value(10);
    BOOST_REQUIRE_EQUAL(10u, p.get_future().get0());
    return make_ready_future();
}

SEASTAR_TEST_CASE(test_finally_waits_for_inner) {
    auto finally = make_shared<bool>();
    auto p = make_shared<promise<>>();

    auto f = make_ready_future().then([] {
    }).finally([=] {
        return p->get_future().then([=] {
            *finally = true;
        });
    }).then([=] {
        BOOST_REQUIRE(*finally);
    });
    BOOST_REQUIRE(!*finally);
    p->set_value();
    return f;
}

SEASTAR_TEST_CASE(test_finally_is_called_on_success_and_failure__not_ready_to_armed) {
    auto finally1 = make_shared<bool>();
    auto finally2 = make_shared<bool>();

    promise<> p;
    auto f = p.get_future().finally([=] {
        *finally1 = true;
    }).then([] {
        throw std::runtime_error("");
    }).finally([=] {
        *finally2 = true;
    }).then_wrapped([=] (auto &&f) {
        BOOST_REQUIRE(*finally1);
        BOOST_REQUIRE(*finally2);
        try {
            f.get();
        } catch (...) {} // silence exceptional future ignored messages
    });

    p.set_value();
    return f;
}

SEASTAR_TEST_CASE(test_exception_from_finally_fails_the_target) {
    promise<> pr;
    auto f = pr.get_future().finally([=] {
        throw std::runtime_error("");
    }).then([] {
        BOOST_REQUIRE(false);
    }).then_wrapped([] (auto&& f) {
        try {
            f.get();
        } catch (...) {} // silence exceptional future ignored messages
    });

    pr.set_value();
    return f;
}

SEASTAR_TEST_CASE(test_exception_from_finally_fails_the_target_on_already_resolved) {
    return make_ready_future().finally([=] {
        throw std::runtime_error("");
    }).then([] {
        BOOST_REQUIRE(false);
    }).then_wrapped([] (auto&& f) {
        try {
            f.get();
        } catch (...) {} // silence exceptional future ignored messages
    });
}

SEASTAR_TEST_CASE(test_exception_thrown_from_then_wrapped_causes_future_to_fail) {
    return make_ready_future().then_wrapped([] (auto&& f) {
        throw std::runtime_error("");
    }).then_wrapped([] (auto&& f) {
        try {
            f.get();
            BOOST_REQUIRE(false);
        } catch (...) {}
    });
}

SEASTAR_TEST_CASE(test_exception_thrown_from_then_wrapped_causes_future_to_fail__async_case) {
    promise<> p;

    auto f = p.get_future().then_wrapped([] (auto&& f) {
        throw std::runtime_error("");
    }).then_wrapped([] (auto&& f) {
        try {
            f.get();
            BOOST_REQUIRE(false);
        } catch (...) {}
    });

    p.set_value();

    return f;
}

SEASTAR_TEST_CASE(test_failing_intermediate_promise_should_fail_the_master_future) {
    promise<> p1;
    promise<> p2;

    auto f = p1.get_future().then([f = p2.get_future()] () mutable {
        return std::move(f);
    }).then([] {
        BOOST_REQUIRE(false);
    });

    p1.set_value();
    p2.set_exception(std::runtime_error("boom"));

    return std::move(f).then_wrapped([](auto&& f) {
        try {
            f.get();
            BOOST_REQUIRE(false);
        } catch (...) {}
    });
}

SEASTAR_TEST_CASE(test_future_forwarding__not_ready_to_unarmed) {
    promise<> p1;
    promise<> p2;

    auto f1 = p1.get_future();
    auto f2 = p2.get_future();

    f1.forward_to(std::move(p2));

    BOOST_REQUIRE(!f2.available());

    auto called = f2.then([] {});

    p1.set_value();
    return called;
}

SEASTAR_TEST_CASE(test_future_forwarding__not_ready_to_armed) {
    promise<> p1;
    promise<> p2;

    auto f1 = p1.get_future();
    auto f2 = p2.get_future();

    auto called = f2.then([] {});

    f1.forward_to(std::move(p2));

    BOOST_REQUIRE(!f2.available());

    p1.set_value();

    return called;
}

SEASTAR_TEST_CASE(test_future_forwarding__ready_to_unarmed) {
    promise<> p2;

    auto f1 = make_ready_future<>();
    auto f2 = p2.get_future();

    std::move(f1).forward_to(std::move(p2));
    BOOST_REQUIRE(f2.available());

    return std::move(f2).then_wrapped([] (future<> f) {
        BOOST_REQUIRE(!f.failed());
    });
}

SEASTAR_TEST_CASE(test_future_forwarding__ready_to_armed) {
    promise<> p2;

    auto f1 = make_ready_future<>();
    auto f2 = p2.get_future();

    auto called = std::move(f2).then([] {});

    BOOST_REQUIRE(f1.available());

    f1.forward_to(std::move(p2));
    return called;
}

static void forward_dead_unarmed_promise_with_dead_future_to(promise<>& p) {
    promise<> p2;
    p.get_future().forward_to(std::move(p2));
}

SEASTAR_TEST_CASE(test_future_forwarding__ready_to_unarmed_soon_to_be_dead) {
    promise<> p1;
    forward_dead_unarmed_promise_with_dead_future_to(p1);
    make_ready_future<>().forward_to(std::move(p1));
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_exception_can_be_thrown_from_do_until_body) {
    return do_until([] { return false; }, [] {
        throw expected_exception();
        return now();
    }).then_wrapped([] (auto&& f) {
       try {
           f.get();
           BOOST_FAIL("should have failed");
       } catch (const expected_exception& e) {
           // expected
       }
    });
}

SEASTAR_TEST_CASE(test_bare_value_can_be_returned_from_callback) {
    return now().then([] {
        return 3;
    }).then([] (int x) {
        BOOST_REQUIRE(x == 3);
    });
}

SEASTAR_TEST_CASE(test_when_all_iterator_range) {
    std::vector<future<size_t>> futures;
    for (size_t i = 0; i != 1000000; ++i) {
        // .then() usually returns a ready future, but sometimes it
        // doesn't, so call it a million times.  This exercises both
        // available and unavailable paths in when_all().
        futures.push_back(make_ready_future<>().then([i] { return i; }));
    }
    // Verify the above statement is correct
    BOOST_REQUIRE(!std::all_of(futures.begin(), futures.end(),
            [] (auto& f) { return f.available(); }));
    auto p = make_shared(std::move(futures));
    return when_all(p->begin(), p->end()).then([p] (std::vector<future<size_t>> ret) {
        BOOST_REQUIRE(std::all_of(ret.begin(), ret.end(), [] (auto& f) { return f.available(); }));
        BOOST_REQUIRE(std::all_of(ret.begin(), ret.end(), [&ret] (auto& f) { return std::get<0>(f.get()) == size_t(&f - ret.data()); }));
    });
}

SEASTAR_TEST_CASE(test_map_reduce) {
    auto square = [] (long x) { return make_ready_future<long>(x*x); };
    long n = 1000;
    return map_reduce(boost::make_counting_iterator<long>(0), boost::make_counting_iterator<long>(n),
            square, long(0), std::plus<long>()).then([n] (auto result) {
        auto m = n - 1; // counting does not include upper bound
        BOOST_REQUIRE_EQUAL(result, (m * (m + 1) * (2*m + 1)) / 6);
    });
}

// This test doesn't actually test anything - it just waits for the future
// returned by sleep to complete. However, a bug we had in sleep() caused
// this test to fail the sanitizer in the debug build, so this is a useful
// regression test.
SEASTAR_TEST_CASE(test_sleep) {
    return sleep(std::chrono::milliseconds(100));
}

SEASTAR_TEST_CASE(test_do_with_1) {
    return do_with(1, [] (int& one) {
       BOOST_REQUIRE_EQUAL(one, 1);
       return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(test_do_with_2) {
    return do_with(1, 2L, [] (int& one, long two) {
        BOOST_REQUIRE_EQUAL(one, 1);
        BOOST_REQUIRE_EQUAL(two, 2);
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(test_do_with_3) {
    return do_with(1, 2L, 3, [] (int& one, long two, int three) {
        BOOST_REQUIRE_EQUAL(one, 1);
        BOOST_REQUIRE_EQUAL(two, 2);
        BOOST_REQUIRE_EQUAL(three, 3);
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(test_do_with_4) {
    return do_with(1, 2L, 3, 4, [] (int& one, long two, int three, int four) {
        BOOST_REQUIRE_EQUAL(one, 1);
        BOOST_REQUIRE_EQUAL(two, 2);
        BOOST_REQUIRE_EQUAL(three, 3);
        BOOST_REQUIRE_EQUAL(four, 4);
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(test_do_while_stopping_immediately) {
    return do_with(int(0), [] (int& count) {
        return repeat([&count] {
            ++count;
            return stop_iteration::yes;
        }).then([&count] {
            BOOST_REQUIRE(count == 1);
        });
    });
}

SEASTAR_TEST_CASE(test_do_while_stopping_after_two_iterations) {
    return do_with(int(0), [] (int& count) {
        return repeat([&count] {
            ++count;
            return count == 2 ? stop_iteration::yes : stop_iteration::no;
        }).then([&count] {
            BOOST_REQUIRE(count == 2);
        });
    });
}

SEASTAR_TEST_CASE(test_do_while_failing_in_the_first_step) {
    return repeat([] {
        throw expected_exception();
        return stop_iteration::no;
    }).then_wrapped([](auto&& f) {
        try {
            f.get();
            BOOST_FAIL("should not happen");
        } catch (const expected_exception&) {
            // expected
        }
    });
}

SEASTAR_TEST_CASE(test_do_while_failing_in_the_second_step) {
    return do_with(int(0), [] (int& count) {
        return repeat([&count] {
            ++count;
            if (count > 1) {
                throw expected_exception();
            }
            return later().then([] { return stop_iteration::no; });
        }).then_wrapped([&count](auto&& f) {
            try {
                f.get();
                BOOST_FAIL("should not happen");
            } catch (const expected_exception&) {
                BOOST_REQUIRE(count == 2);
            }
        });
    });
}

SEASTAR_TEST_CASE(test_parallel_for_each) {
    return async([] {
        // empty
        parallel_for_each(std::vector<int>(), [] (int) -> future<> {
            BOOST_FAIL("should not reach");
            abort();
        }).get();

        // immediate result
        auto range = boost::copy_range<std::vector<int>>(boost::irange(1, 6));
        auto sum = 0;
        parallel_for_each(range, [&sum] (int v) {
            sum += v;
            return make_ready_future<>();
        }).get();
        BOOST_REQUIRE_EQUAL(sum, 15);

        // all suspend
        sum = 0;
        parallel_for_each(range, [&sum] (int v) {
            return later().then([&sum, v] {
                sum += v;
            });
        }).get();
        BOOST_REQUIRE_EQUAL(sum, 15);

        // throws immediately
        BOOST_CHECK_EXCEPTION(parallel_for_each(range, [] (int) -> future<> {
            throw 5;
        }).get(), int, [] (int v) { return v == 5; });

        // throws after suspension
        BOOST_CHECK_EXCEPTION(parallel_for_each(range, [] (int) {
            return later().then([] {
                throw 5;
            });
        }).get(), int, [] (int v) { return v == 5; });
    });
}

SEASTAR_TEST_CASE(test_parallel_for_each_early_failure) {
    return do_with(0, [] (int& counter) {
        return parallel_for_each(boost::irange(0, 11000), [&counter] (int i) {
            using namespace std::chrono_literals;
            // force scheduling
            return sleep((i % 31 + 1) * 1ms).then([&counter, i] {
                ++counter;
                if (i % 1777 == 1337) {
                    return make_exception_future<>(i);
                }
                return make_ready_future<>();
            });
        }).then_wrapped([&counter] (future<> f) {
            BOOST_REQUIRE_EQUAL(counter, 11000);
            BOOST_REQUIRE(f.failed());
            try {
                f.get();
                BOOST_FAIL("wanted an exception");
            } catch (int i) {
                BOOST_REQUIRE(i % 1777 == 1337);
            } catch (...) {
                BOOST_FAIL("bad exception type");
            }
        });
    });
}

SEASTAR_TEST_CASE(test_parallel_for_each_waits_for_all_fibers_even_if_one_of_them_failed) {
    auto can_exit = make_lw_shared<bool>(false);
    return parallel_for_each(boost::irange(0, 2), [can_exit] (int i) {
        return later().then([i, can_exit] {
            if (i == 1) {
                throw expected_exception();
            } else {
                using namespace std::chrono_literals;
                return sleep(300ms).then([can_exit] {
                    *can_exit = true;
                });
            }
        });
    }).then_wrapped([can_exit] (auto&& f) {
        try {
            f.get();
        } catch (...) {
            // expected
        }
        BOOST_REQUIRE(*can_exit);
    });
}

#ifndef SEASTAR_SHUFFLE_TASK_QUEUE
SEASTAR_TEST_CASE(test_high_priority_task_runs_before_ready_continuations) {
    return now().then([] {
        auto flag = make_lw_shared<bool>(false);
        engine().add_high_priority_task(make_task([flag] {
            *flag = true;
        }));
        return make_ready_future().then([flag] {
            BOOST_REQUIRE(*flag);
        });
    });
}

SEASTAR_TEST_CASE(test_high_priority_task_runs_in_the_middle_of_loops) {
    auto counter = make_lw_shared<int>(0);
    auto flag = make_lw_shared<bool>(false);
    return repeat([counter, flag] {
        if (*counter == 1) {
            BOOST_REQUIRE(*flag);
            return stop_iteration::yes;
        }
        engine().add_high_priority_task(make_task([flag] {
            *flag = true;
        }));
        ++(*counter);
        return stop_iteration::no;
    });
}
#endif

SEASTAR_TEST_CASE(futurize_invoke_val_exception) {
    return futurize_invoke([] (int arg) { throw expected_exception(); return arg; }, 1).then_wrapped([] (future<int> f) {
        try {
            f.get();
            BOOST_FAIL("should have thrown");
        } catch (expected_exception& e) {}
    });
}

SEASTAR_TEST_CASE(futurize_invoke_val_ok) {
    return futurize_invoke([] (int arg) { return arg * 2; }, 2).then_wrapped([] (future<int> f) {
        try {
            auto x = f.get0();
            BOOST_REQUIRE_EQUAL(x, 4);
        } catch (expected_exception& e) {
            BOOST_FAIL("should not have thrown");
        }
    });
}

SEASTAR_TEST_CASE(futurize_invoke_val_future_exception) {
    return futurize_invoke([] (int a) {
        return sleep(std::chrono::milliseconds(100)).then([] {
            throw expected_exception();
            return make_ready_future<int>(0);
        });
    }, 0).then_wrapped([] (future<int> f) {
        try {
            f.get();
            BOOST_FAIL("should have thrown");
        } catch (expected_exception& e) { }
    });
}

SEASTAR_TEST_CASE(futurize_invoke_val_future_ok) {
    return futurize_invoke([] (int a) {
        return sleep(std::chrono::milliseconds(100)).then([a] {
            return make_ready_future<int>(a * 100);
        });
    }, 2).then_wrapped([] (future<int> f) {
        try {
            auto x = f.get0();
            BOOST_REQUIRE_EQUAL(x, 200);
        } catch (expected_exception& e) {
            BOOST_FAIL("should not have thrown");
        }
    });
}
SEASTAR_TEST_CASE(futurize_invoke_void_exception) {
    return futurize_invoke([] (auto arg) { throw expected_exception(); }, 0).then_wrapped([] (future<> f) {
        try {
            f.get();
            BOOST_FAIL("should have thrown");
        } catch (expected_exception& e) {}
    });
}

SEASTAR_TEST_CASE(futurize_invoke_void_ok) {
    return futurize_invoke([] (auto arg) { }, 0).then_wrapped([] (future<> f) {
        try {
            f.get();
        } catch (expected_exception& e) {
            BOOST_FAIL("should not have thrown");
        }
    });
}

SEASTAR_TEST_CASE(futurize_invoke_void_future_exception) {
    return futurize_invoke([] (auto a) {
        return sleep(std::chrono::milliseconds(100)).then([] {
            throw expected_exception();
        });
    }, 0).then_wrapped([] (future<> f) {
        try {
            f.get();
            BOOST_FAIL("should have thrown");
        } catch (expected_exception& e) { }
    });
}

SEASTAR_TEST_CASE(futurize_invoke_void_future_ok) {
    auto a = make_lw_shared<int>(1);
    return futurize_invoke([] (int& a) {
        return sleep(std::chrono::milliseconds(100)).then([&a] {
            a *= 100;
        });
    }, *a).then_wrapped([a] (future<> f) {
        try {
            f.get();
            BOOST_REQUIRE_EQUAL(*a, 100);
        } catch (expected_exception& e) {
            BOOST_FAIL("should not have thrown");
        }
    });
}

SEASTAR_TEST_CASE(test_unused_shared_future_is_not_a_broken_future) {
    promise<> p;
    shared_future<> s(p.get_future());
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_shared_future_propagates_value_to_all) {
    return seastar::async([] {
        promise<shared_ptr<int>> p; // shared_ptr<> to check it deals with emptyable types
        shared_future<shared_ptr<int>> f(p.get_future());

        auto f1 = f.get_future();
        auto f2 = f.get_future();

        p.set_value(make_shared<int>(1));
        BOOST_REQUIRE(*f1.get0() == 1);
        BOOST_REQUIRE(*f2.get0() == 1);
    });
}

template<typename... T>
void check_fails_with_expected(future<T...> f) {
    try {
        f.get();
        BOOST_FAIL("Should have failed");
    } catch (expected_exception&) {
        // expected
    }
}

SEASTAR_TEST_CASE(test_shared_future_propagates_value_to_copies) {
    return seastar::async([] {
        promise<int> p;
        auto sf1 = shared_future<int>(p.get_future());
        auto sf2 = sf1;

        auto f1 = sf1.get_future();
        auto f2 = sf2.get_future();

        p.set_value(1);

        BOOST_REQUIRE(f1.get0() == 1);
        BOOST_REQUIRE(f2.get0() == 1);
    });
}

SEASTAR_TEST_CASE(test_obtaining_future_from_shared_future_after_it_is_resolved) {
    promise<int> p1;
    promise<int> p2;
    auto sf1 = shared_future<int>(p1.get_future());
    auto sf2 = shared_future<int>(p2.get_future());
    p1.set_value(1);
    p2.set_exception(expected_exception());
    return sf2.get_future().then_wrapped([f1 = sf1.get_future()] (auto&& f) mutable {
        check_fails_with_expected(std::move(f));
        return std::move(f1);
    }).then_wrapped([] (auto&& f) {
        BOOST_REQUIRE(f.get0() == 1);
    });
}

SEASTAR_TEST_CASE(test_valueless_shared_future) {
    return seastar::async([] {
        promise<> p;
        shared_future<> f(p.get_future());

        auto f1 = f.get_future();
        auto f2 = f.get_future();

        p.set_value();

        f1.get();
        f2.get();
    });
}

SEASTAR_TEST_CASE(test_shared_future_propagates_errors_to_all) {
    promise<int> p;
    shared_future<int> f(p.get_future());

    auto f1 = f.get_future();
    auto f2 = f.get_future();

    p.set_exception(expected_exception());

    return f1.then_wrapped([f2 = std::move(f2)] (auto&& f) mutable {
        check_fails_with_expected(std::move(f));
        return std::move(f2);
    }).then_wrapped([] (auto&& f) mutable {
        check_fails_with_expected(std::move(f));
    });
}

SEASTAR_TEST_CASE(test_ignored_future_warning) {
    // This doesn't warn:
    promise<> p;
    p.set_exception(expected_exception());
    future<> f = p.get_future();
    f.ignore_ready_future();

    // And by analogy, neither should this
    shared_promise<> p2;
    p2.set_exception(expected_exception());
    future<> f2 = p2.get_shared_future();
    f2.ignore_ready_future();
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_futurize_from_tuple) {
    std::tuple<int> v1 = std::make_tuple(3);
    std::tuple<> v2 = {};
    BOOST_REQUIRE(futurize<int>::from_tuple(v1).get() == v1);
    BOOST_REQUIRE(futurize<void>::from_tuple(v2).get() == v2);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_repeat_until_value) {
    return do_with(int(), [] (int& counter) {
        return repeat_until_value([&counter] () -> future<compat::optional<int>> {
            if (counter == 10000) {
                return make_ready_future<compat::optional<int>>(counter);
            } else {
                ++counter;
                return make_ready_future<compat::optional<int>>(compat::nullopt);
            }
        }).then([&counter] (int result) {
            BOOST_REQUIRE(counter == 10000);
            BOOST_REQUIRE(result == counter);
        });
    });
}

SEASTAR_TEST_CASE(test_repeat_until_value_implicit_future) {
    // Same as above, but returning compat::optional<int> instead of future<compat::optional<int>>
    return do_with(int(), [] (int& counter) {
        return repeat_until_value([&counter] {
            if (counter == 10000) {
                return compat::optional<int>(counter);
            } else {
                ++counter;
                return compat::optional<int>(compat::nullopt);
            }
        }).then([&counter] (int result) {
            BOOST_REQUIRE(counter == 10000);
            BOOST_REQUIRE(result == counter);
        });
    });
}

SEASTAR_TEST_CASE(test_repeat_until_value_exception) {
    return repeat_until_value([] {
        throw expected_exception();
        return compat::optional<int>(43);
    }).then_wrapped([] (future<int> f) {
        check_fails_with_expected(std::move(f));
    });
}

SEASTAR_TEST_CASE(test_when_allx) {
    return when_all(later(), later(), make_ready_future()).discard_result();
}

#if __cplusplus >= 201703L

// A noncopyable and nonmovable struct
struct non_copy_non_move {
    non_copy_non_move() = default;
    non_copy_non_move(non_copy_non_move&&) = delete;
    non_copy_non_move(const non_copy_non_move&) = delete;
};

SEASTAR_TEST_CASE(test_when_all_functions) {
    auto f = [x = non_copy_non_move()] {
        (void)x;
        return make_ready_future<int>(42);
    };
    return when_all(f, [] {
        throw 42;
        return make_ready_future<>();
    }, later()).then([] (std::tuple<future<int>, future<>, future<>> res) {
        BOOST_REQUIRE_EQUAL(std::get<0>(res).get0(), 42);

        BOOST_REQUIRE(std::get<1>(res).available());
        BOOST_REQUIRE(std::get<1>(res).failed());
        std::get<1>(res).ignore_ready_future();

        BOOST_REQUIRE(std::get<2>(res).available());
        BOOST_REQUIRE(!std::get<2>(res).failed());
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(test_when_all_succeed_functions) {
    auto f = [x = non_copy_non_move()] {
        (void)x;
        return make_ready_future<int>(42);
    };
    return when_all_succeed(f, [] {
        throw 42;
        return make_ready_future<>();
    }, later()).then_wrapped([] (future<int> res) {
        BOOST_REQUIRE(res.available());
        BOOST_REQUIRE(res.failed());
        res.ignore_ready_future();
        return make_ready_future<>();
    });
}

#endif

template<typename E, typename... T>
static void check_failed_with(future<T...>&& f) {
    BOOST_REQUIRE(f.failed());
    try {
        f.get();
        BOOST_FAIL("exception expected");
    } catch (const E& e) {
        // expected
    } catch (...) {
        BOOST_FAIL(format("wrong exception: {}", std::current_exception()));
    }
}

template<typename... T>
static void check_timed_out(future<T...>&& f) {
    check_failed_with<timed_out_error>(std::move(f));
}

SEASTAR_TEST_CASE(test_with_timeout_when_it_times_out) {
    return seastar::async([] {
        promise<> pr;
        auto f = with_timeout(manual_clock::now() + 2s, pr.get_future());

        BOOST_REQUIRE(!f.available());

        manual_clock::advance(1s);
        later().get();

        BOOST_REQUIRE(!f.available());

        manual_clock::advance(1s);
        later().get();

        check_timed_out(std::move(f));

        pr.set_value();
    });
}

SEASTAR_THREAD_TEST_CASE(test_shared_future_get_future_after_timeout) {
    // This used to crash because shared_future checked if the list of
    // pending futures was empty to decide if it had already called
    // then_wrapped. If all pending futures timed out, it would call
    // it again.
    promise<> pr;
    shared_future<with_clock<manual_clock>> sfut(pr.get_future());
    future<> fut1 = sfut.get_future(manual_clock::now() + 1s);

    manual_clock::advance(1s);

    check_timed_out(std::move(fut1));

    future<> fut2 = sfut.get_future(manual_clock::now() + 1s);
    manual_clock::advance(1s);
    check_timed_out(std::move(fut2));

    future<> fut3 = sfut.get_future(manual_clock::now() + 1s);
    pr.set_value();
}

SEASTAR_TEST_CASE(test_custom_exception_factory_in_with_timeout) {
    return seastar::async([] {
        class custom_error : public std::exception {
        public:
            virtual const char* what() const noexcept {
                return "timedout";
            }
        };
        struct my_exception_factory {
            static auto timeout() {
                return custom_error();
            }
        };
        promise<> pr;
        auto f = with_timeout<my_exception_factory>(manual_clock::now() + 1s, pr.get_future());

        manual_clock::advance(1s);
        later().get();

        check_failed_with<custom_error>(std::move(f));
    });
}

SEASTAR_TEST_CASE(test_with_timeout_when_it_does_not_time_out) {
    return seastar::async([] {
        {
            promise<int> pr;
            auto f = with_timeout(manual_clock::now() + 1s, pr.get_future());

            pr.set_value(42);

            BOOST_REQUIRE_EQUAL(f.get0(), 42);
        }

        // Check that timer was indeed cancelled
        manual_clock::advance(1s);
        later().get();
    });
}

SEASTAR_TEST_CASE(test_shared_future_with_timeout) {
    return seastar::async([] {
        shared_promise<with_clock<manual_clock>, int> pr;
        auto f1 = pr.get_shared_future(manual_clock::now() + 1s);
        auto f2 = pr.get_shared_future(manual_clock::now() + 2s);
        auto f3 = pr.get_shared_future();

        BOOST_REQUIRE(!f1.available());
        BOOST_REQUIRE(!f2.available());
        BOOST_REQUIRE(!f3.available());

        manual_clock::advance(1s);
        later().get();

        check_timed_out(std::move(f1));
        BOOST_REQUIRE(!f2.available());
        BOOST_REQUIRE(!f3.available());

        manual_clock::advance(1s);
        later().get();

        check_timed_out(std::move(f2));
        BOOST_REQUIRE(!f3.available());

        pr.set_value(42);

        BOOST_REQUIRE_EQUAL(42, f3.get0());
    });
}

SEASTAR_TEST_CASE(test_when_all_succeed_tuples) {
    return seastar::when_all_succeed(
        make_ready_future<>(),
        make_ready_future<sstring>("hello world"),
        make_ready_future<int>(42),
        make_ready_future<>(),
        make_ready_future<int, sstring>(84, "hi"),
        make_ready_future<bool>(true)
    ).then([] (sstring msg, int v, std::tuple<int, sstring> t, bool b) {
        BOOST_REQUIRE_EQUAL(msg, "hello world");
        BOOST_REQUIRE_EQUAL(v, 42);
        BOOST_REQUIRE_EQUAL(std::get<0>(t), 84);
        BOOST_REQUIRE_EQUAL(std::get<1>(t), "hi");
        BOOST_REQUIRE_EQUAL(b, true);

        return seastar::when_all_succeed(
                make_exception_future<>(42),
                make_ready_future<sstring>("hello world"),
                make_exception_future<int>(43),
                make_ready_future<>()
        ).then([] (sstring, int) {
            BOOST_FAIL("shouldn't reach");
            return false;
        }).handle_exception([] (auto excp) {
            try {
                std::rethrow_exception(excp);
            } catch (int v) {
                BOOST_REQUIRE(v == 42 || v == 43);
                return true;
            } catch (...) { }
            return false;
        }).then([] (auto ret) {
            BOOST_REQUIRE(ret);
        });
    });
}

SEASTAR_TEST_CASE(test_when_all_succeed_vector) {
    std::vector<future<>> vecs;
    vecs.emplace_back(make_ready_future<>());
    vecs.emplace_back(make_ready_future<>());
    vecs.emplace_back(make_ready_future<>());
    vecs.emplace_back(make_ready_future<>());
    return seastar::when_all_succeed(vecs.begin(), vecs.end()).then([] {
        std::vector<future<>> vecs;
        vecs.emplace_back(make_ready_future<>());
        vecs.emplace_back(make_ready_future<>());
        vecs.emplace_back(make_exception_future<>(42));
        vecs.emplace_back(make_exception_future<>(43));
        return seastar::when_all_succeed(vecs.begin(), vecs.end());
    }).then([] {
        BOOST_FAIL("shouldn't reach");
        return false;
    }).handle_exception([] (auto excp) {
        try {
            std::rethrow_exception(excp);
        } catch (int v) {
            BOOST_REQUIRE(v == 42 || v == 43);
            return true;
        } catch (...) { }
        return false;
    }).then([] (auto ret) {
        BOOST_REQUIRE(ret);

        std::vector<future<int>> vecs;
        vecs.emplace_back(make_ready_future<int>(1));
        vecs.emplace_back(make_ready_future<int>(2));
        vecs.emplace_back(make_ready_future<int>(3));
        return seastar::when_all_succeed(vecs.begin(), vecs.end());
    }).then([] (std::vector<int> vals) {
        BOOST_REQUIRE_EQUAL(vals.size(), 3u);
        BOOST_REQUIRE_EQUAL(vals[0], 1);
        BOOST_REQUIRE_EQUAL(vals[1], 2);
        BOOST_REQUIRE_EQUAL(vals[2], 3);

        std::vector<future<int>> vecs;
        vecs.emplace_back(make_ready_future<int>(1));
        vecs.emplace_back(make_ready_future<int>(2));
        vecs.emplace_back(make_exception_future<int>(42));
        vecs.emplace_back(make_exception_future<int>(43));
        return seastar::when_all_succeed(vecs.begin(), vecs.end());
    }).then([] (std::vector<int>) {
        BOOST_FAIL("shouldn't reach");
        return false;
    }).handle_exception([] (auto excp) {
        try {
            std::rethrow_exception(excp);
        } catch (int v) {
            BOOST_REQUIRE(v == 42 || v == 43);
            return true;
        } catch (...) { }
        return false;
    }).then([] (auto ret) {
        BOOST_REQUIRE(ret);
    });
}

SEASTAR_TEST_CASE(test_futurize_mutable) {
    int count = 0;
    return seastar::repeat([count]() mutable {
        ++count;
        if (count == 3) {
            return seastar::stop_iteration::yes;
        }
        return seastar::stop_iteration::no;
    });
}

SEASTAR_THREAD_TEST_CASE(test_broken_promises) {
    compat::optional<future<>> f;
    compat::optional<future<>> f2;
    { // Broken after attaching a continuation
        auto p = promise<>();
        f = p.get_future();
        f2 = f->then_wrapped([&] (future<> f3) {
            BOOST_CHECK(f3.failed());
            BOOST_CHECK_THROW(f3.get(), broken_promise);
            f = { };
        });
    }
    f2->get();
    BOOST_CHECK(!f);

    { // Broken before attaching a continuation
        auto p = promise<>();
        f = p.get_future();
    }
    f->then_wrapped([&] (future<> f3) {
        BOOST_CHECK(f3.failed());
        BOOST_CHECK_THROW(f3.get(), broken_promise);
        f = { };
    }).get();
    BOOST_CHECK(!f);

    { // Broken before suspending a thread
        auto p = promise<>();
        f = p.get_future();
    }
    BOOST_CHECK_THROW(f->get(), broken_promise);
}

SEASTAR_TEST_CASE(test_warn_on_broken_promise_with_no_future) {
    // Example code where we expect a "Exceptional future ignored"
    // warning. We can't directly test that the warning is issued, but
    // this example functions as documentation.
    promise<> p;
    // Intentionally destroy the future
    (void)p.get_future();
    p.set_exception(std::runtime_error("foo"));
    return make_ready_future<>();
}

SEASTAR_THREAD_TEST_CASE(test_exception_future_with_backtrace) {
    int counter = 0;
    auto inner = [&] (bool return_exception) mutable {
        if (!return_exception) {
            return make_ready_future<int>(++counter);
        } else {
            return make_exception_future_with_backtrace<int>(expected_exception());
        }
    };
    auto outer = [&] (bool return_exception) {
        return inner(return_exception).then([] (int i) {
            return make_ready_future<int>(-i);
        });
    };

    BOOST_REQUIRE_EQUAL(outer(false).get0(), -1);
    BOOST_REQUIRE_EQUAL(counter, 1);

    BOOST_CHECK_THROW(outer(true).get0(), expected_exception);
    BOOST_REQUIRE_EQUAL(counter, 1);

    // Example code where we expect a "Exceptional future ignored"
    // warning. We can't directly test that the warning is issued, but
    // this example functions as documentation.
    (void)outer(true);
}

future<> func4() {
    return later().then([] {
        seastar_logger.info("backtrace: {}", current_backtrace());
    });
}

void func3() {
    seastar::async([] {
        func4().get();
    }).get();
}

future<> func2() {
    return seastar::async([] {
        func3();
    });
}

future<> func1() {
    return later().then([] {
        return func2();
    });
}

SEASTAR_THREAD_TEST_CASE(test_backtracing) {
    func1().get();
}
