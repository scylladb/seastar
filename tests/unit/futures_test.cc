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

#include <boost/test/tools/old/interface.hpp>
#include <cstddef>
#include <forward_list>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <seastar/testing/test_case.hh>

#include <seastar/core/reactor.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/stream.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/manual_clock.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/print.hh>
#include <seastar/core/when_any.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/gate.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/log.hh>
#include <seastar/util/later.hh>
#include <boost/iterator/counting_iterator.hpp>
#include <seastar/testing/thread_test_case.hh>

#include <boost/range/iterator_range.hpp>
#include <boost/range/irange.hpp>

#include <seastar/core/internal/api-level.hh>
#include <unistd.h>

#include "expected_exception.hh"

using namespace seastar;
using namespace std::chrono_literals;

static_assert(std::is_nothrow_default_constructible_v<gate>,
    "seastar::gate constructor must not throw");
static_assert(std::is_nothrow_move_constructible_v<gate>,
    "seastar::gate move constructor must not throw");

static_assert(std::is_nothrow_default_constructible_v<shared_future<>>);
static_assert(std::is_nothrow_copy_constructible_v<shared_future<>>);
static_assert(std::is_nothrow_move_constructible_v<shared_future<>>);

static_assert(std::is_nothrow_move_constructible_v<shared_promise<>>);

#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 13)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wself-move"
#endif
SEASTAR_TEST_CASE(test_self_move) {
    future_state<std::tuple<std::unique_ptr<int>>> s1;
    s1.set(std::make_unique<int>(42));
    s1 = std::move(s1); // no crash, but the value of s1 is not defined.

    future_state<std::unique_ptr<int>> s2;
    s2.set(std::make_unique<int>(42));
    std::swap(s2, s2);
    BOOST_REQUIRE_EQUAL(*std::move(s2).get(), 42);

    promise<std::unique_ptr<int>> p1;
    p1.set_value(std::make_unique<int>(42));
    p1 = std::move(p1); // no crash, but the value of p1 is not defined.

    promise<std::unique_ptr<int>> p2;
    p2.set_value(std::make_unique<int>(42));
    std::swap(p2, p2);
    BOOST_REQUIRE_EQUAL(*p2.get_future().get(), 42);

    auto  f1 = make_ready_future<std::unique_ptr<int>>(std::make_unique<int>(42));
    f1 = std::move(f1); // no crash, but the value of f1 is not defined.

    auto f2 = make_ready_future<std::unique_ptr<int>>(std::make_unique<int>(42));
    std::swap(f2, f2);
    BOOST_REQUIRE_EQUAL(*f2.get(), 42);

    return make_ready_future<>();
}
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 13)
#pragma GCC diagnostic pop
#endif

static subscription<int> get_empty_subscription(std::function<future<> (int)> func) {
    stream<int> s;
    auto ret = s.listen(func);
    s.close();
    return ret;
}

struct int_container {
    int_container() = default;
    int_container(int_container&&) noexcept = default;
    // this template can be matched by an initializer like `{foo}`, which was used in
    // uninitialized_wrapper_base::uninitialized_set() to perform placement new.
    template <typename T>
    int_container(const std::vector<T>&) {
        static_assert(std::is_constructible_v<int, T>);
    }
};

SEASTAR_TEST_CASE(test_future_value_constructible_from_range) {
    // verify that the type a future's value is constructible from a range
    using vector_type = std::vector<int_container>;
    std::ignore = seastar::make_ready_future<vector_type>(vector_type{});
    return make_ready_future();
}

SEASTAR_TEST_CASE(test_stream) {
    auto sub = get_empty_subscription([](int x) {
        return make_ready_future<>();
    });
    return sub.done();
}

SEASTAR_TEST_CASE(test_stream_drop_sub) {
    auto s = make_lw_shared<stream<int>>();
    const int expected = 42;
    std::optional<future<>> ret;
    {
        auto sub = s->listen([expected](int actual) {
            BOOST_REQUIRE_EQUAL(expected, actual);
            return make_ready_future<>();
        });
        ret = sub.done();
        // It is ok to drop the subscription when we only want the competition future.
    }
    return s->produce(expected).then([ret = std::move(*ret), s] () mutable {
        s->close();
        return std::move(ret);
    });
}

SEASTAR_TEST_CASE(test_reference) {
    int a = 42;
    future<int&> orig = make_ready_future<int&>(a);
    future<int&> fut = std::move(orig);
    int& r = fut.get();
    r = 43;
    BOOST_REQUIRE_EQUAL(a, 43);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_set_future_state_with_tuple) {
    future_state<std::tuple<int>> s1;
    promise<int> p1;
    const std::tuple<int> v1(42);
    s1.set(v1);
    p1.set_value(v1);

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
    BOOST_REQUIRE_EQUAL(10u, p.get_future().get());
    return make_ready_future();
}

// An exception class with a controlled what() overload
class test_exception : public std::exception {
    sstring _what;
public:
    explicit test_exception(sstring what) : _what(std::move(what)) {}
    virtual const char* what() const noexcept override {
        return _what.c_str();
    }
};

SEASTAR_TEST_CASE(test_get_on_exceptional_promise) {
    auto p = promise<>();
    p.set_exception(test_exception("test"));
    BOOST_REQUIRE_THROW(p.get_future().get(), test_exception);
    return make_ready_future();
}

static void check_finally_exception(std::exception_ptr ex) {
  BOOST_REQUIRE_EQUAL(fmt::format("{}", ex),
        "seastar::nested_exception: test_exception (bar) (while cleaning up after test_exception (foo))");
  try {
      // convert to the concrete type nested_exception
      std::rethrow_exception(ex);
  } catch (seastar::nested_exception& ex) {
    try {
        std::rethrow_exception(ex.inner);
    } catch (test_exception& inner) {
        BOOST_REQUIRE_EQUAL(inner.what(), "bar");
    }
    try {
        ex.rethrow_nested();
    } catch (test_exception& outer) {
        BOOST_REQUIRE_EQUAL(outer.what(), "foo");
    }
  }
}

SEASTAR_TEST_CASE(test_finally_exception) {
    return make_ready_future<>().then([] {
        throw test_exception("foo");
    }).finally([] {
        throw test_exception("bar");
    }).handle_exception(check_finally_exception);
}

SEASTAR_TEST_CASE(test_finally_exceptional_future) {
    return make_ready_future<>().then([] {
        throw test_exception("foo");
    }).finally([] {
       return make_exception_future<>(test_exception("bar"));
    }).handle_exception(check_finally_exception);
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

SEASTAR_TEST_CASE(test_exception_can_be_thrown_from_do_until_condition) {
    return do_until([] { throw expected_exception(); return false; }, [] {
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
        // Use a mix of available and unavailable futures to exercise
        // both paths in when_all().
        auto fut = (i % 2) == 0 ? make_ready_future<>() : yield();
        futures.push_back(fut.then([i] { return i; }));
    }
    // Verify the above statement is correct
    BOOST_REQUIRE(!std::all_of(futures.begin(), futures.end(),
            [] (auto& f) { return f.available(); }));
    auto p = make_shared(std::move(futures));
    return when_all(p->begin(), p->end()).then([p] (std::vector<future<size_t>> ret) {
        BOOST_REQUIRE(std::all_of(ret.begin(), ret.end(), [] (auto& f) { return f.available(); }));
        BOOST_REQUIRE(std::all_of(ret.begin(), ret.end(), [&ret] (auto& f) { return f.get() == size_t(&f - ret.data()); }));
    });
}

template<typename Container>
void test_iterator_range_estimate() {
    Container container{1,2,3};

    BOOST_REQUIRE_EQUAL(internal::iterator_range_estimate_vector_capacity(
        container.begin(), container.end()), 3);
}

BOOST_AUTO_TEST_CASE(test_iterator_range_estimate_vector_capacity) {
    test_iterator_range_estimate<std::vector<int>>();
    test_iterator_range_estimate<std::list<int>>();
    test_iterator_range_estimate<std::forward_list<int>>();
    {
        int n = 42;
        auto seq = std::views::iota(0, n);
        BOOST_REQUIRE_EQUAL(internal::iterator_range_estimate_vector_capacity(
            seq.begin(), seq.end()), n);
    }
    {
        // for ranges that generate elements on-the-fly, advancing an iterator
        // might actually consume or transform the underlying sequence, in this
        // case, the function under test returns 0.
        auto seq = std::views::iota(1);
        BOOST_REQUIRE_EQUAL(internal::iterator_range_estimate_vector_capacity(
            seq.begin(), seq.end()), 0);
    }
}

// helper function for when_any tests
template<typename Container>
future<> when_all_but_one_succeed(Container& futures, size_t leave_out)
{
    auto sz = futures.size();
    SEASTAR_ASSERT(sz >= 1);
    SEASTAR_ASSERT(leave_out < sz);
    std::vector<future<size_t>> all_but_one_tmp;
    all_but_one_tmp.reserve(sz - 1);
    for (size_t i = 0 ; i < sz; i++){
        if (i == leave_out) { continue; }
        all_but_one_tmp.push_back(std::move(futures[i]));
    }
    auto all_but_one = make_shared(std::move(all_but_one_tmp));
    return when_all_succeed(all_but_one->begin(), all_but_one->end()).then([all_but_one] (auto&& _) {
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(test_when_any_iterator_range_i) {
    std::vector<future<size_t>> futures;
    for (size_t i = 0; i != 100; ++i) {
        auto fut = yield();
        futures.push_back(fut.then([i] { return i; }));
    }

    // Verify the above statement is correct
    BOOST_REQUIRE(std::all_of(futures.begin(), futures.end(), [](auto &f) { return !f.available(); }));

    auto p = make_shared(std::move(futures));
    return seastar::when_any(p->begin(), p->end()).then([p](auto &&ret_obj) {
        BOOST_REQUIRE(ret_obj.futures[ret_obj.index].available());
        BOOST_REQUIRE(ret_obj.futures[ret_obj.index].get() == ret_obj.index);
        return when_all_but_one_succeed(ret_obj.futures, ret_obj.index);
    });
}

SEASTAR_TEST_CASE(test_when_any_iterator_range_ii) {
    std::vector<future<size_t>> futures;
    for (size_t i = 0; i != 100; ++i) {
        if (i == 42) {
            auto fut = seastar::make_ready_future<>();
            futures.push_back(fut.then([i] { return i; }));
        } else {
            auto fut = seastar::sleep(100ms);
            futures.push_back(fut.then([i] { return i; }));
        }
    }
    auto p = make_shared(std::move(futures));
    return seastar::when_any(p->begin(), p->end()).then([p](auto &&ret_obj) {
        BOOST_REQUIRE(ret_obj.futures[ret_obj.index].available());
        BOOST_REQUIRE(ret_obj.futures[ret_obj.index].get() == ret_obj.index);
        BOOST_REQUIRE(ret_obj.index == 42);
        return when_all_but_one_succeed(ret_obj.futures, ret_obj.index);
    });
}

SEASTAR_TEST_CASE(test_when_any_iterator_range_iii) {
    std::vector<future<size_t>> futures;
    for (size_t i = 0; i != 100; ++i) {
        if (i == 42) {
            auto fut = seastar::sleep(5ms);
            futures.push_back(fut.then([i] { return i; }));
        } else {
            auto fut = seastar::sleep(100ms);
            futures.push_back(fut.then([i] { return i; }));
        }
    }
    auto p = make_shared(std::move(futures));
    return seastar::when_any(p->begin(), p->end()).then([p](auto &&ret_obj) {
        BOOST_REQUIRE(ret_obj.futures[ret_obj.index].available());
        BOOST_REQUIRE(ret_obj.futures[ret_obj.index].get() == ret_obj.index);
        BOOST_REQUIRE(ret_obj.index == 42);
        return when_all_but_one_succeed(ret_obj.futures, ret_obj.index);
    });
}

SEASTAR_TEST_CASE(test_when_any_iterator_range_iv) {
    std::vector<future<size_t>> futures;
    for (size_t i = 0; i != 100; ++i) {
        if (i == 42) {
            auto fut = yield().then([] { return seastar::make_exception_future(std::runtime_error("test")); } );
            futures.push_back(fut.then([i] { return i; }));
        } else {
            auto fut = seastar::sleep(100ms);
            futures.push_back(fut.then([i] { return i; }));
        }
    }
    auto p = make_shared(std::move(futures));
    return seastar::when_any(p->begin(), p->end()).then([p](auto &&ret_obj) {
        BOOST_REQUIRE(ret_obj.futures[ret_obj.index].available());
        BOOST_REQUIRE_THROW(ret_obj.futures[ret_obj.index].get(), std::runtime_error);
        return when_all_but_one_succeed(ret_obj.futures, ret_obj.index);
    });
}

SEASTAR_TEST_CASE(test_when_any_variadic_i)
{
    auto f_int = yield().then([] { return make_ready_future<int>(42); });
    auto f_string = sleep(100ms).then([] { return make_ready_future<sstring>("hello"); });
    auto f_l33tspeak = sleep(100ms).then([] {
        return make_ready_future<std::tuple<char, int, int, char, char, int, char>>(
            std::make_tuple('s', 3, 4, 's', 't', 4, 'r'));
    });
    return when_any(std::move(f_int), std::move(f_string), std::move(f_l33tspeak)).then([](auto&& wa_result) {
        BOOST_REQUIRE(wa_result.index == 0);
        auto [one, two, three] = std::move(wa_result.futures);
        BOOST_REQUIRE(one.get() == 42);
        return when_all_succeed(std::move(two), std::move(three)).then([](auto _) { return seastar::make_ready_future<>(); });
    });
}

SEASTAR_TEST_CASE(test_when_any_variadic_ii)
{
    struct foo {
        int bar = 86;
    };

    auto f_int = sleep(100ms).then([] { return make_ready_future<int>(42); });
    auto f_foo = sleep(75ms).then([] { return make_ready_future<foo>(); });
    auto f_string = sleep(1ms).then([] { return make_ready_future<sstring>("hello"); });
    auto f_l33tspeak = sleep(50ms).then([] {
        return make_ready_future<std::tuple<char, int, int, char, char, int, char>>(
            std::make_tuple('s', 3, 4, 's', 't', 4, 'r'));
    });
    return when_any(std::move(f_int), std::move(f_foo), std::move(f_string), std::move(f_l33tspeak))
        .then([](auto&& wa_result) {
            BOOST_REQUIRE(wa_result.index == 2);
            auto [one, two, three, four] = std::move(wa_result.futures);
            BOOST_REQUIRE(three.get() == "hello");
            return when_any(std::move(one), std::move(two), std::move(four)).then([](auto wa_nextresult) {
                auto [one, two, four] = std::move(wa_nextresult.futures);
                BOOST_REQUIRE(wa_nextresult.index == 2);
                BOOST_REQUIRE(four.get() == std::make_tuple('s', 3, 4, 's', 't', 4, 'r'));
                return when_any(std::move(one), std::move(two)).then([](auto wa_result) {
                    auto [one, two] = std::move(wa_result.futures);
                    BOOST_REQUIRE(wa_result.index == 1);
                    BOOST_REQUIRE(two.get().bar == foo{}.bar);
                    return one.then([](int x) { BOOST_REQUIRE(x == 42); });
                });
            });
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

SEASTAR_TEST_CASE(test_map_reduce_simple) {
    return do_with(0L, [] (auto& res) {
        long n = 10;
        return map_reduce(boost::make_counting_iterator<long>(0), boost::make_counting_iterator<long>(n),
                [] (long x) { return x; },
                [&res] (long x) { res += x; }).then([n, &res] {
            long expected = (n * (n - 1)) / 2;
            BOOST_REQUIRE_EQUAL(res, expected);
        });
    });
}

SEASTAR_TEST_CASE(test_map_reduce_tuple) {
    return do_with(0L, 0L, [] (auto& res0, auto& res1) {
        long n = 10;
        return map_reduce(boost::make_counting_iterator<long>(0), boost::make_counting_iterator<long>(n),
                [] (long x) { return std::tuple<long, long>(x, -x); },
                [&res0, &res1] (std::tuple<long, long> t) { res0 += std::get<0>(t); res1 += std::get<1>(t); }).then([n, &res0, &res1] {
            long expected = (n * (n - 1)) / 2;
            BOOST_REQUIRE_EQUAL(res0, expected);
            BOOST_REQUIRE_EQUAL(res1, -expected);
        });
    });
}

SEASTAR_TEST_CASE(test_map_reduce_lifetime) {
    struct map {
        bool destroyed = false;
        map() = default;
        map(const map&) = default;
        ~map() {
            destroyed = true;
        }
        auto operator()(long x) {
            return yield().then([this, x] {
                BOOST_REQUIRE(!destroyed);
                return x;
            });
        }
    };
    struct reduce {
        long& res;
        bool destroyed = false;
        reduce(long& result)
            : res{result} {}
        reduce(const reduce&) = default;
        ~reduce() {
            destroyed = true;
        }
        auto operator()(long x) {
            return yield().then([this, x] {
                BOOST_REQUIRE(!destroyed);
                res += x;
            });
        }
    };
    return do_with(0L, [] (auto& res) {
        long n = 10;
        return map_reduce(boost::make_counting_iterator<long>(0), boost::make_counting_iterator<long>(n),
                map{}, reduce{res}).then([n, &res] {
            long expected = (n * (n - 1)) / 2;
            BOOST_REQUIRE_EQUAL(res, expected);
        });
    });
}

SEASTAR_TEST_CASE(test_map_reduce0_lifetime) {
    struct map {
        bool destroyed = false;
        map() = default;
        map(const map&) = default;
        ~map() {
            destroyed = true;
        }
        auto operator()(long x) {
            return yield().then([this, x] {
                BOOST_REQUIRE(!destroyed);
                return x;
            });
        }
    };
    struct reduce {
        bool destroyed = false;
        reduce() = default;
        reduce(const reduce&) = default;
        ~reduce() {
            destroyed = true;
        }
        auto operator()(long res, long x) {
            BOOST_REQUIRE(!destroyed);
            return res + x;
        }
    };
    long n = 10;
    return map_reduce(boost::make_counting_iterator<long>(0), boost::make_counting_iterator<long>(n),
            map{}, 0L, reduce{}).then([n] (long res) {
        long expected = (n * (n - 1)) / 2;
        BOOST_REQUIRE_EQUAL(res, expected);
    });
}

SEASTAR_TEST_CASE(test_map_reduce1_lifetime) {
    struct map {
        bool destroyed = false;
        map() = default;
        map(const map&) = default;
        ~map() {
            destroyed = true;
        }
        auto operator()(long x) {
            return yield().then([this, x] {
                BOOST_REQUIRE(!destroyed);
                return x;
            });
        }
    };
    struct reduce {
        long res = 0;
        bool destroyed = false;
        reduce() = default;
        reduce(const reduce&) = default;
        ~reduce() {
            BOOST_TEST_MESSAGE("~reduce()");
            destroyed = true;
        }
        auto operator()(long x) {
            return yield().then([this, x] {
                BOOST_REQUIRE(!destroyed);
                res += x;
                return make_ready_future<>();
            });
        }
        auto get() {
            return sleep(std::chrono::milliseconds(10)).then([this] {
                BOOST_REQUIRE(!destroyed);
                return res;
            });
        }
    };
    long n = 10;
    return map_reduce(boost::make_counting_iterator<long>(0), boost::make_counting_iterator<long>(n),
                      map{}, reduce{}).then([n] (long res) {
        long expected = (n * (n - 1)) / 2;
        BOOST_REQUIRE_EQUAL(res, expected);
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

SEASTAR_TEST_CASE(test_do_with_5) {
    using func = noncopyable_function<void()>;
    return do_with(func([] {}), [] (func&) {
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(test_do_with_6) {
    const int x = 42;
    return do_with(int(42), x, [](int&, int&) {
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(test_do_with_7) {
    const int x = 42;
    return do_with(x, [](int&) {
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
            return yield().then([] { return stop_iteration::no; });
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
            return yield().then([&sum, v] {
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
            return yield().then([] {
                throw 5;
            });
        }).get(), int, [] (int v) { return v == 5; });
    });
}

SEASTAR_TEST_CASE(test_parallel_for_each_early_failure) {
    return do_with(0, [] (int& counter) {
        return parallel_for_each(std::views::iota(0, 11000), [&counter] (int i) {
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
    return parallel_for_each(std::views::iota(0, 2), [can_exit] (int i) {
        return yield().then([i, can_exit] {
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

SEASTAR_THREAD_TEST_CASE(test_parallel_for_each_broken_promise) {
    auto fut = [] {
        std::vector<promise<>> v(2);
        return parallel_for_each(v, [] (promise<>& p) {
            return p.get_future();
        });
    }();
    BOOST_CHECK_THROW(fut.get(), broken_promise);
}

SEASTAR_THREAD_TEST_CASE(test_repeat_broken_promise) {
    auto get_fut = [] {
        promise<stop_iteration> pr;
        return pr.get_future();
    };

    future<> r = repeat([fut = get_fut()] () mutable {
        return std::move(fut);
    });

    BOOST_CHECK_THROW(r.get(), broken_promise);
}

#ifndef SEASTAR_SHUFFLE_TASK_QUEUE
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
            auto x = f.get();
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
            auto x = f.get();
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
        BOOST_REQUIRE(*f1.get() == 1);
        BOOST_REQUIRE(*f2.get() == 1);
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

        BOOST_REQUIRE(f1.get() == 1);
        BOOST_REQUIRE(f2.get() == 1);
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
        BOOST_REQUIRE(f.get() == 1);
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
    future<int> fut1 = futurize<int>::from_tuple(v1);
    future<> fut2 = futurize<void>::from_tuple(v2);
    BOOST_REQUIRE(fut1.get() == std::get<0>(v1));
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_repeat_until_value) {
    return do_with(int(), [] (int& counter) {
        return repeat_until_value([&counter] () -> future<std::optional<int>> {
            if (counter == 10000) {
                return make_ready_future<std::optional<int>>(counter);
            } else {
                ++counter;
                return make_ready_future<std::optional<int>>(std::nullopt);
            }
        }).then([&counter] (int result) {
            BOOST_REQUIRE(counter == 10000);
            BOOST_REQUIRE(result == counter);
        });
    });
}

SEASTAR_TEST_CASE(test_repeat_until_value_implicit_future) {
    // Same as above, but returning std::optional<int> instead of future<std::optional<int>>
    return do_with(int(), [] (int& counter) {
        return repeat_until_value([&counter] {
            if (counter == 10000) {
                return std::optional<int>(counter);
            } else {
                ++counter;
                return std::optional<int>(std::nullopt);
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
        return std::optional<int>(43);
    }).then_wrapped([] (future<int> f) {
        check_fails_with_expected(std::move(f));
    });
}

SEASTAR_TEST_CASE(test_when_allx) {
    return when_all(yield(), yield(), make_ready_future()).discard_result();
}

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
    }, yield()).then([] (std::tuple<future<int>, future<>, future<>> res) {
        BOOST_REQUIRE_EQUAL(std::get<0>(res).get(), 42);

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
    }, yield()).then_wrapped([] (future<std::tuple<int>> res) {
        BOOST_REQUIRE(res.available());
        BOOST_REQUIRE(res.failed());
        res.ignore_ready_future();
        return make_ready_future<>();
    });
}

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
        yield().get();

        BOOST_REQUIRE(!f.available());

        manual_clock::advance(1s);
        yield().get();

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
    fut3.get();
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
        yield().get();

        check_failed_with<custom_error>(std::move(f));
    });
}

SEASTAR_TEST_CASE(test_with_timeout_when_it_does_not_time_out) {
    return seastar::async([] {
        {
            promise<int> pr;
            auto f = with_timeout(manual_clock::now() + 1s, pr.get_future());

            pr.set_value(42);

            BOOST_REQUIRE_EQUAL(f.get(), 42);
        }

        // Check that timer was indeed cancelled
        manual_clock::advance(1s);
        yield().get();
    });
}

template<typename... T>
static void check_aborted(future<T...>&& f) {
    check_failed_with<abort_requested_exception>(std::move(f));
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
        yield().get();

        check_timed_out(std::move(f1));
        BOOST_REQUIRE(!f2.available());
        BOOST_REQUIRE(!f3.available());

        manual_clock::advance(1s);
        yield().get();

        check_timed_out(std::move(f2));
        BOOST_REQUIRE(!f3.available());

        pr.set_value(42);

        BOOST_REQUIRE_EQUAL(42, f3.get());
    });
}

SEASTAR_THREAD_TEST_CASE(test_shared_future_with_abort) {
    abort_source as;
    abort_source as2;
    shared_promise<with_clock<manual_clock>, int> pr;
    auto f1 = pr.get_shared_future(as);
    auto f2 = pr.get_shared_future(as2);
    auto f3 = pr.get_shared_future();

    BOOST_REQUIRE(!f1.available());
    BOOST_REQUIRE(!f2.available());
    BOOST_REQUIRE(!f3.available());

    as.request_abort();

    check_aborted(std::move(f1));
    BOOST_REQUIRE(!f2.available());
    BOOST_REQUIRE(!f3.available());

    as2.request_abort();

    check_aborted(std::move(f2));
    BOOST_REQUIRE(!f3.available());

    pr.set_value(42);

    BOOST_REQUIRE_EQUAL(42, f3.get());

    auto f4 = pr.get_shared_future(as);
    BOOST_REQUIRE(f4.available());
}

SEASTAR_THREAD_TEST_CASE(test_shared_promise_with_outstanding_future_is_immediately_available) {
    shared_promise<> pr1;
    auto f1 = pr1.get_shared_future();
    pr1.set_value();
    BOOST_REQUIRE(pr1.available());
    BOOST_REQUIRE_NO_THROW(f1.get());

    shared_promise<> pr2;
    auto f2 = pr2.get_shared_future();
    pr2.set_exception(std::runtime_error("oops"));
    BOOST_REQUIRE(pr2.available());
    BOOST_REQUIRE_THROW(f2.get(), std::runtime_error);
}

namespace seastar {
class shared_future_tester {
public:
    template <typename... T>
    static bool has_scheduled_task(const shared_future<T...>& f) noexcept {
        return f._state->has_scheduled_task();
    }
};
}

SEASTAR_THREAD_TEST_CASE(test_shared_future_task_scheduled_only_if_there_are_waiting_futures) {
    {
        // Case 1: promise is eventually satisfied, get_future is not called
        promise<> pr1;
        shared_future<> f1(pr1.get_future());
        BOOST_REQUIRE(!shared_future_tester::has_scheduled_task(f1));

        pr1.set_value();
        // get_future was not called, so no task should have been scheduled
        BOOST_REQUIRE(!shared_future_tester::has_scheduled_task(f1));
    }

    {
        // Case 2: promise is eventually satisfied, get_future was called
        promise<> pr2;
        shared_future<> f2(pr2.get_future());
        auto f2f = f2.get_future();

        // get_future was called, so the task is scheduled to happen after promise is resolved
        BOOST_REQUIRE(shared_future_tester::has_scheduled_task(f2));

        pr2.set_value();
        f2f.get();
        // f2f is resolved by shared future's task, so it must have run
        BOOST_REQUIRE(!shared_future_tester::has_scheduled_task(f2));
    }

    {
        // Case 3: shared future is ready from the start
        shared_future<> f3(make_ready_future<>());
        BOOST_REQUIRE(!shared_future_tester::has_scheduled_task(f3));

        auto f3f = f3.get_future();
        // Calling get_future on a ready shared future does not schedule the task
        BOOST_REQUIRE(!shared_future_tester::has_scheduled_task(f3));
        BOOST_REQUIRE(f3f.available());
    }
}

SEASTAR_TEST_CASE(test_when_all_succeed_tuples) {
    return seastar::when_all_succeed(
        make_ready_future<>(),
        make_ready_future<sstring>("hello world"),
        make_ready_future<int>(42),
        make_ready_future<>(),
        make_ready_future<std::tuple<int, sstring>>(std::tuple(84, "hi")),
        make_ready_future<bool>(true)
    ).then_unpack([] (sstring msg, int v, std::tuple<int, sstring> t, bool b) {
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
        ).then_unpack([] (sstring, int) {
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

SEASTAR_TEST_CASE(test_when_all_succeed_vector_overload) {
    std::vector<future<int>> vecs_noexcept;
    vecs_noexcept.reserve(10);
    for(int i = 0; i < 10; i++) {
        vecs_noexcept.emplace_back(i % 2 == 0 ? make_ready_future<int>(42) : yield().then([] { return 42; }));
    }

    std::vector<future<int>> vecs_except;
    vecs_except.reserve(10);
    for(int i = 0; i < 10; i++) {
        vecs_except.emplace_back(i % 2 == 0 ? make_ready_future<int>(42) : make_exception_future<int>(43));
    }

    return seastar::when_all_succeed(std::move(vecs_noexcept))
    .then([vecs_except = std::move(vecs_except)] (std::vector<int> vals) mutable {
        bool all = std::all_of(vals.cbegin(), vals.cend(), [](int val) { return val == 42; });
        BOOST_REQUIRE(all);
        return seastar::when_all_succeed(std::move(vecs_except));
    }).then_wrapped([] (auto vals_fut) {
        auto vals = vals_fut.get();
        BOOST_FAIL("shouldn't reach");
        return false;
    })
    .handle_exception([] (auto excp) {
        try {
            std::rethrow_exception(excp);
        } catch (int v) {
            BOOST_REQUIRE(v == 43);
            return true;
        } catch (...) { }
        return false;
    }).then([] (auto ret) {
        BOOST_REQUIRE(ret);
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
    std::optional<future<>> f;
    std::optional<future<>> f2;
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
    // warning.
    promise<> p;
    // Intentionally destroy the future
    (void)p.get_future();

    reactor::test::with_allow_abandoned_failed_futures(1, [&] {
        p.set_exception(std::runtime_error("foo"));
    });

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_destroy_promise_after_state_take_value) {
    future<> f = make_ready_future<>();
    auto p = std::make_unique<seastar::promise<>>();
    f = p->get_future();
    p->set_value();
    auto g = f.then([] {});
    p.reset();
    return g;
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

    BOOST_REQUIRE_EQUAL(outer(false).get(), -1);
    BOOST_REQUIRE_EQUAL(counter, 1);

    BOOST_CHECK_THROW(outer(true).get(), expected_exception);
    BOOST_REQUIRE_EQUAL(counter, 1);

    // Example code where we expect a "Exceptional future ignored"
    // warning.
    (void)outer(true).then_wrapped([](future<int> fut) {
        reactor::test::with_allow_abandoned_failed_futures(1, [fut = std::move(fut)]() mutable {
            auto foo = std::move(fut);
        });
    });
}

class throw_on_move {
    int _i;
public:
    throw_on_move(int i = 0) noexcept {
        _i = i;
    }
    throw_on_move(const throw_on_move&) = delete;
    throw_on_move(throw_on_move&&) {
        _i = -1;
        throw expected_exception();
    }

    int value() const {
        return _i;
    }
};

SEASTAR_TEST_CASE(test_async_throw_on_move) {
    return async([] (throw_on_move t) {
        BOOST_CHECK(false);
    }, throw_on_move()).handle_exception_type([] (const expected_exception&) {
        return make_ready_future<>();
    });
}

future<> func4() {
    return yield().then([] {
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
    return yield().then([] {
        return func2();
    });
}

SEASTAR_THREAD_TEST_CASE(test_backtracing) {
    func1().get();
}

SEASTAR_THREAD_TEST_CASE(test_then_unpack) {
    make_ready_future<std::tuple<>>().then_unpack([] () {
        BOOST_REQUIRE(true);
    }).get();
    make_ready_future<std::tuple<int>>(std::tuple<int>(1)).then_unpack([] (int x) {
        BOOST_REQUIRE(x == 1);
    }).get();
    make_ready_future<std::tuple<int, long>>(std::tuple<int, long>(1, 2)).then_unpack([] (int x, long y) {
        BOOST_REQUIRE(x == 1 && y == 2);
    }).get();
    make_ready_future<std::tuple<std::unique_ptr<int>>>(std::tuple(std::make_unique<int>(42))).then_unpack([] (std::unique_ptr<int> p1) {
        BOOST_REQUIRE(*p1 == 42);
    }).get();
}

future<> test_then_function_f() {
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_then_function) {
    return make_ready_future<>().then(test_then_function_f);
}

SEASTAR_THREAD_TEST_CASE(test_with_gate) {
    gate g;
    int counter = 0;
    int gate_closed_errors = 0;
    int other_errors = 0;

    // test normal operation when gate is opened
    BOOST_CHECK_NO_THROW(with_gate(g, [&] { counter++; }).get());
    BOOST_REQUIRE_EQUAL(counter, 1);

    // test that an exception returned by the calling func
    // is propagated to with_gate future
    counter = gate_closed_errors = other_errors = 0;
    BOOST_CHECK_NO_THROW(with_gate(g, [&] {
            counter++;
            return make_exception_future<>(expected_exception());
        }).handle_exception_type([&] (gate_closed_exception& e) {
            gate_closed_errors++;
        }).handle_exception([&] (std::exception_ptr) {
            other_errors++;
        }).get());
    BOOST_REQUIRE(counter);
    BOOST_REQUIRE(!gate_closed_errors);
    BOOST_REQUIRE(other_errors);

    g.close().get();

    // test that with_gate.get() throws when the gate is closed
    counter = gate_closed_errors = other_errors = 0;
    BOOST_CHECK_THROW(with_gate(g, [&] { counter++; }).get(), gate_closed_exception);
    BOOST_REQUIRE(!counter);

    // test that with_gate throws when the gate is closed
    counter = gate_closed_errors = other_errors = 0;
    BOOST_CHECK_THROW(with_gate(g, [&] {
            counter++;
        }).then_wrapped([&] (future<> f) {
            auto eptr = f.get_exception();
            try {
                std::rethrow_exception(eptr);
            } catch (gate_closed_exception& e) {
                gate_closed_errors++;
            } catch (...) {
                other_errors++;
            }
        }).get(), gate_closed_exception);
    BOOST_REQUIRE(!counter);
    BOOST_REQUIRE(!gate_closed_errors);
    BOOST_REQUIRE(!other_errors);

    // test that try_with_gate returns gate_closed_exception when the gate is closed
    counter = gate_closed_errors = other_errors = 0;
    try_with_gate(g, [&] { counter++; }).handle_exception_type([&] (gate_closed_exception& e) {
        gate_closed_errors++;
    }).handle_exception([&] (std::exception_ptr) {
        other_errors++;
    }).get();
    BOOST_REQUIRE(!counter);
    BOOST_REQUIRE(gate_closed_errors);
    BOOST_REQUIRE(!other_errors);
}

SEASTAR_THREAD_TEST_CASE(test_max_concurrent_for_each) {
    BOOST_TEST_MESSAGE("empty range");
    max_concurrent_for_each(std::vector<int>(), 3, [] (int) {
        BOOST_FAIL("should not reach");
        return make_exception_future<>(std::bad_function_call());
    }).get();

    auto range = boost::copy_range<std::vector<int>>(boost::irange(1, 8));

    BOOST_TEST_MESSAGE("iterator");
    auto sum = 0;
    max_concurrent_for_each(range.begin(), range.end(), 3,  [&sum] (int v) {
        sum += v;
        return make_ready_future<>();
    }).get();
    BOOST_REQUIRE_EQUAL(sum, 28);

    BOOST_TEST_MESSAGE("const iterator");
    sum = 0;
    max_concurrent_for_each(range.cbegin(), range.cend(), 3,  [&sum] (int v) {
        sum += v;
        return make_ready_future<>();
    }).get();
    BOOST_REQUIRE_EQUAL(sum, 28);

    BOOST_TEST_MESSAGE("reverse iterator");
    sum = 0;
    max_concurrent_for_each(range.rbegin(), range.rend(), 3,  [&sum] (int v) {
        sum += v;
        return make_ready_future<>();
    }).get();
    BOOST_REQUIRE_EQUAL(sum, 28);

    BOOST_TEST_MESSAGE("immediate result");
    sum = 0;
    max_concurrent_for_each(range, 3,  [&sum] (int v) {
        sum += v;
        return make_ready_future<>();
    }).get();
    BOOST_REQUIRE_EQUAL(sum, 28);

    BOOST_TEST_MESSAGE("suspend");
    sum = 0;
    max_concurrent_for_each(range, 3, [&sum] (int v) {
        return yield().then([&sum, v] {
            sum += v;
        });
    }).get();
    BOOST_REQUIRE_EQUAL(sum, 28);

    BOOST_TEST_MESSAGE("throw immediately");
    sum = 0;
    BOOST_CHECK_EXCEPTION(max_concurrent_for_each(range, 3, [&sum] (int v) {
        sum += v;
        if (v == 1) {
            throw 5;
        }
        return make_ready_future<>();
    }).get(), int, [] (int v) { return v == 5; });
    BOOST_REQUIRE_EQUAL(sum, 28);

    BOOST_TEST_MESSAGE("throw after suspension");
    sum = 0;
    BOOST_CHECK_EXCEPTION(max_concurrent_for_each(range, 3, [&sum] (int v) {
        return yield().then([&sum, v] {
            sum += v;
            if (v == 2) {
                throw 5;
            }
        });
    }).get(), int, [] (int v) { return v == 5; });

    BOOST_TEST_MESSAGE("concurrency higher than vector length");
    sum = 0;
    max_concurrent_for_each(range, range.size() + 3,  [&sum] (int v) {
        sum += v;
        return make_ready_future<>();
    }).get();
    BOOST_REQUIRE_EQUAL(sum, 28);
}

SEASTAR_THREAD_TEST_CASE(test_for_each_set) {
    std::bitset<32> s;
    s.set(4);
    s.set(0);

    auto range = bitsets::for_each_set(s);
    unsigned res = 0;
    do_for_each(range, [&res] (auto i) {
        res |= 1 << i;
    }).get();
    BOOST_REQUIRE_EQUAL(res, 17);
}

SEASTAR_THREAD_TEST_CASE(test_yield) {
    bool flag = false;
    auto one = yield().then([&] {
        flag = true;
    });
    BOOST_REQUIRE_EQUAL(flag, false);
    one.get();
    BOOST_REQUIRE_EQUAL(flag, true);

#ifndef SEASTAR_DEBUG
    // same thing, with now(), but for non-DEBUG only, otherwise .then() doesn't
    // use the ready-future fast-path and always schedules a task
    flag = false;
    auto two = now().then([&] {
        flag = true;
    });
    // now() does not yield
    BOOST_REQUIRE_EQUAL(flag, true);
#endif
}

// The seastar::make_exception_future() function has two distinct cases - it
// can create an exceptional future from an existing std::exception_ptr, or
// from an any object which will be wrapped in an std::exception_ptr using
// std::make_exception_ptr. We want to test here these two cases, as well
// what happens when the given parameter is almost a std::exception_ptr,
// just with different qualifiers, like && or const (see issue #1010).
SEASTAR_TEST_CASE(test_make_exception_future) {
    // When make_exception_future() is given most types - like int and
    // std::runtime_error - a copy of the given value get stored in the
    // future (internally, it is wrapped using std::make_exception_ptr):
    future<> f1 = make_exception_future<>(3);
    BOOST_REQUIRE(f1.failed());
    BOOST_REQUIRE_THROW(f1.get(), int);
    future<> f2 = make_exception_future<>(std::runtime_error("hello"));
    BOOST_REQUIRE(f2.failed());
    BOOST_REQUIRE_THROW(f2.get(), std::runtime_error);
    // However, if make_exception_future() is given an std::exception_ptr
    // it behaves differently - the exception stored in the future will be
    // the one held in the given exception_ptr - not the exception_ptr object
    // itself.
    std::exception_ptr e3 = std::make_exception_ptr(3);
    future<> f3 = make_exception_future<>(e3);
    BOOST_REQUIRE(f3.failed());
    BOOST_REQUIRE_THROW(f3.get(), int); // expecting int, not std::exception_ptr
    // If make_exception_future() is given an std::exception_ptr by rvalue,
    // it should also work correctly:
    // An unnamed rvalue:
    future<> f4 = make_exception_future<>(std::make_exception_ptr(3));
    BOOST_REQUIRE(f4.failed());
    BOOST_REQUIRE_THROW(f4.get(), int); // expecting int, not std::exception_ptr
    // A rvalue reference (a move):
    std::exception_ptr e5 = std::make_exception_ptr(3);
    future<> f5 = make_exception_future<>(std::move(e5)); // note std::move()
    BOOST_REQUIRE(f5.failed());
    BOOST_REQUIRE_THROW(f5.get(), int); // expecting int, not std::exception_ptr
    // A rvalue reference to a *const* exception_ptr:
    // Reproduces issue #1010 - a const exception_ptr sounds odd, but can
    // happen accidentally when capturing an exception_ptr in a non-mutable
    // lambda.
    // Note that C++ is fine with std::move() being used on a const object,
    // it will simply fall back to a copy instead of a move. And a copy does
    // work (without std::move(), it works).
    const std::exception_ptr e6 = std::make_exception_ptr(3); // note const!
    future<> f6 = make_exception_future<>(std::move(e6)); // note std::move()
    BOOST_REQUIRE(f6.failed());
    BOOST_REQUIRE_THROW(f6.get(), int); // expecting int, not std::exception_ptr

    return make_ready_future<>();
}

// Reproduce use-after-free similar to #1514
SEASTAR_TEST_CASE(test_run_in_background) {
    engine().run_in_background([] {
        return sleep(1ms).then([] {
            return smp::invoke_on_all([] {
                return sleep(1ms);
            });
        });
    });
    return make_ready_future<>();
}

SEASTAR_THREAD_TEST_CASE(test_manual_clock_advance) {
    bool expired = false;
    auto t = timer<manual_clock>([&] {
        expired = true;
    });
    t.arm(2ms);
    manual_clock::advance(1ms);
    BOOST_REQUIRE(!expired);
    manual_clock::advance(1ms);
    BOOST_REQUIRE(expired);
}

SEASTAR_THREAD_TEST_CASE(test_ready_future_across_shards) {
    if (smp::count == 1) {
        seastar_logger.info("test_ready_future_across_shards requires at least 2 shards");
        return;
    }

    auto other_shard = (this_shard_id() + 1) % smp::count;
    auto f1 = make_ready_future<int>(42);
    smp::submit_to(other_shard, [f1 = std::move(f1)] () mutable {
        BOOST_REQUIRE_EQUAL(f1.get(), 42);
    }).get();
}

SEASTAR_THREAD_TEST_CASE(test_foreign_promise_set_value) {
    if (smp::count == 1) {
        seastar_logger.info("test_foreign_promise_set_value requires at least 2 shards");
        return;
    }

    promise<int> pr;
    auto other_shard = (this_shard_id() + 1) % smp::count;

    auto getter = pr.get_future();

    auto setter = smp::submit_to(other_shard, [&] {
        pr.set_value(this_shard_id());
    });

    setter.get();

    BOOST_REQUIRE_EQUAL(getter.get(), other_shard);
}
