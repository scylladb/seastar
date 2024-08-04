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
 * Copyright (C) 2024 ScyllaDB Ltd.
 */

#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/coroutine/generator.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/util/bool_class.hh>
#include <seastar/util/later.hh>
#include <string_view>
#if __cplusplus >= 202302L && defined(__cpp_lib_generator)
#include <generator>
template <typename T>
using sync_generator = std::generator<const T&>;
#else
#include "tl-generator.hh"
template <typename T>
using sync_generator = tl::generator<T>;
#endif

using namespace seastar;

using do_suspend = bool_class<struct do_suspend_tag>;

sync_generator<int>
sync_fibonacci_sequence(unsigned count) {
    auto a = 0, b = 1;
    for (unsigned i = 0; i < count; ++i) {
        if (std::numeric_limits<decltype(a)>::max() - a < b) {
            throw std::out_of_range(
                fmt::format("fibonacci[{}] is greater than the largest value of int", i));
        }
        int next = std::exchange(a, std::exchange(b, a + b));
        // tl::generator::yield_value() only accepts arguments of reference type,
        // instead of a cv-qualified value.
        co_yield next;
    }
}

coroutine::experimental::generator<int>
async_fibonacci_sequence(unsigned count, do_suspend suspend) {
    auto a = 0, b = 1;
    for (unsigned i = 0; i < count; ++i) {
        if (std::numeric_limits<decltype(a)>::max() - a < b) {
            throw std::out_of_range(
                fmt::format("fibonacci[{}] is greater than the largest value of int", i));
        }
        if (suspend) {
            co_await yield();
        }
        co_yield std::exchange(a, std::exchange(b, a + b));
    }
}

seastar::future<>
verify_fib_drained(coroutine::experimental::generator<int> actual_fibs, unsigned count) {
    auto expected_fibs = sync_fibonacci_sequence(count);
    auto expected_fib = std::begin(expected_fibs);

    auto actual_fib = co_await actual_fibs.begin();

    for (; actual_fib != actual_fibs.end(); co_await ++actual_fib) {
        BOOST_REQUIRE(expected_fib != std::end(expected_fibs));
        BOOST_REQUIRE_EQUAL(*actual_fib, *expected_fib);
        ++expected_fib;
    }
    BOOST_REQUIRE(actual_fib == actual_fibs.end());
}

SEASTAR_TEST_CASE(test_generator_drained_with_suspend) {
    constexpr int count = 4;
    return verify_fib_drained(async_fibonacci_sequence(count, do_suspend::yes),
                              count);
}

SEASTAR_TEST_CASE(test_generator_drained_without_suspend) {
    constexpr int count = 4;
    return verify_fib_drained(async_fibonacci_sequence(count, do_suspend::no),
                              count);
}

seastar::future<> test_generator_not_drained(do_suspend suspend) {
    auto fib = async_fibonacci_sequence(42, suspend);
    auto actual_fib = co_await fib.begin();
    BOOST_REQUIRE_EQUAL(*actual_fib, 0);
}

SEASTAR_TEST_CASE(test_generator_not_drained_with_suspend) {
    return test_generator_not_drained(do_suspend::yes);
}

SEASTAR_TEST_CASE(test_generator_not_drained_without_suspend) {
    return test_generator_not_drained(do_suspend::no);
}

coroutine::experimental::generator<std::string_view, std::string>
generate_value_and_ref(std::vector<std::string_view> strings) {
    co_yield "[";
    std::string s;
    for (auto sv : strings) {
        s = sv;
        s.push_back(',');
        co_yield s;
    }
    co_yield "]";
}

SEASTAR_TEST_CASE(test_generator_value_reference) {
    using namespace std::literals;
    std::vector<std::string_view> expected_quoted = {"["sv, "foo,"sv, "bar,"sv, "]"sv};
    auto actual_quoted = generate_value_and_ref({"foo"sv, "bar"sv});
    auto actual = co_await actual_quoted.begin();
    for (auto expected : expected_quoted) {
        BOOST_REQUIRE_EQUAL(*actual, expected);
        co_await ++actual;
    }
}

coroutine::experimental::generator<std::string>
generate_yield_rvalue_reference(const std::vector<std::string> strings) {
    for (auto& s: strings) {
        co_yield s;
    }
}

SEASTAR_TEST_CASE(test_generator_rvalue_reference) {
    std::vector<std::string> expected_strings = {"hello", "world"};
    auto actual_strings = generate_yield_rvalue_reference(expected_strings);
    auto actual = co_await actual_strings.begin();
    for (auto expected : expected_strings) {
        BOOST_REQUIRE_EQUAL(*actual, expected);
        co_await ++actual;
    }
}

SEASTAR_TEST_CASE(test_generator_move_ctor) {
    constexpr int count = 4;
    auto actual_fibs = async_fibonacci_sequence(count, do_suspend::no);
    return verify_fib_drained(std::move(actual_fibs), count);
}

SEASTAR_TEST_CASE(test_generator_swap) {
    int count_a = 4;
    int count_b = 42;
    auto fibs_a = async_fibonacci_sequence(count_a, do_suspend::no);
    auto fibs_b = async_fibonacci_sequence(count_b, do_suspend::no);
    std::swap(fibs_a, fibs_b);
    std::swap(count_a, count_b);
    co_await verify_fib_drained(std::move(fibs_a), count_a);
    co_await verify_fib_drained(std::move(fibs_b), count_b);
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

coroutine::experimental::generator<counter_t>
fiddle(int n, int* total) {
    int i = 0;
    while (true) {
        if (i++ == n) {
            throw std::invalid_argument("Eureka from generator!");
        }
        co_yield counter_t{i, total};
    }
}

SEASTAR_TEST_CASE(test_generator_throws_from_generator) {
    int total = 0;
    auto count_to = [total=&total](unsigned n) -> seastar::future<> {
        auto count = fiddle(n, total);
        auto it = co_await count.begin();
        for (unsigned i = 0; i < 2 * n; i++) {
            co_await ++it;
        }
    };
    auto f = co_await coroutine::as_future(count_to(42));
    BOOST_REQUIRE(f.failed());
    BOOST_REQUIRE_THROW(std::rethrow_exception(f.get_exception()), std::invalid_argument);
    BOOST_REQUIRE_EQUAL(total, 0);
}

SEASTAR_TEST_CASE(test_generator_throws_from_consumer) {
    int total = 0;
    auto count_to = [total=&total](unsigned n) -> seastar::future<> {
        auto count = fiddle(n, total);
        auto it = co_await count.begin();
        for (unsigned i = 0; i < n; i++) {
            if (i == n / 2) {
                throw std::invalid_argument("Eureka from consumer!");
            }
            co_await ++it;
        }
    };
    auto f = co_await coroutine::as_future(count_to(42));
    BOOST_REQUIRE(f.failed());
    BOOST_REQUIRE_THROW(std::rethrow_exception(f.get_exception()), std::invalid_argument);
    BOOST_REQUIRE_EQUAL(total, 0);
}
