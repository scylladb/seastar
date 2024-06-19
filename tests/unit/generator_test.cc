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
#include <seastar/coroutine/async_generator.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/util/bool_class.hh>

using namespace seastar;

using do_suspend = bool_class<struct do_suspend_tag>;

coroutine::experimental::async_generator<int>
fibonacci_sequence(unsigned count,
                   do_suspend suspend) {
    auto a = 0, b = 1;
    for (unsigned i = 0; i < count; ++i) {
        if (std::numeric_limits<decltype(a)>::max() - a < b) {
            throw std::out_of_range(
                fmt::format("fibonacci[{}] is greater than the largest value of int", i));
        }
        if (suspend) {
            co_await coroutine::maybe_yield();
        }
        co_yield std::exchange(a, std::exchange(b, a + b));
    }
}

seastar::future<> test_async_generator_drained(do_suspend suspend) {
    auto expected_fibs = {0, 1, 1, 2};
    auto expected_fib = std::begin(expected_fibs);

    auto actual_fibs = fibonacci_sequence(std::size(expected_fibs), suspend);
    auto actual_fib = co_await actual_fibs.begin();

    for (; actual_fib != actual_fibs.end(); co_await ++actual_fib) {
        BOOST_REQUIRE(expected_fib != std::end(expected_fibs));
        BOOST_REQUIRE_EQUAL(*actual_fib, *expected_fib);
        ++expected_fib;
    }
    BOOST_REQUIRE(actual_fib == actual_fibs.end());
}

SEASTAR_TEST_CASE(test_async_generator_drained_with_suspend) {
    return test_async_generator_drained(do_suspend::yes);
}

SEASTAR_TEST_CASE(test_async_generator_drained_without_suspend) {
    return test_async_generator_drained(do_suspend::no);
}

seastar::future<> test_async_generator_not_drained(do_suspend suspend) {
    auto fib = fibonacci_sequence(42, suspend);
    auto actual_fib = co_await fib.begin();
    BOOST_REQUIRE_EQUAL(*actual_fib, 0);
}

SEASTAR_TEST_CASE(test_async_generator_not_drained_with_suspend) {
    return test_async_generator_not_drained(do_suspend::yes);
}

SEASTAR_TEST_CASE(test_async_generator_not_drained_without_suspend) {
    return test_async_generator_not_drained(do_suspend::no);
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

coroutine::experimental::async_generator<counter_t>
fiddle(int n, int* total) {
    int i = 0;
    while (true) {
        if (i++ == n) {
            throw std::invalid_argument("Eureka from generator!");
        }
        co_yield counter_t{i, total};
    }
}

SEASTAR_TEST_CASE(test_async_generator_throws_from_generator) {
    int total = 0;
    auto count_to = [total=&total](unsigned n) -> seastar::future<> {
        auto count = fiddle(n, total);
        auto it = co_await count.begin();
        for (unsigned i = 0; i < 2 * n; i++) {
            co_await ++it;
        }
    };
    co_await count_to(42).then_wrapped([&total] (auto f) {
        BOOST_REQUIRE(f.failed());
        BOOST_REQUIRE_THROW(std::rethrow_exception(f.get_exception()), std::invalid_argument);
        BOOST_REQUIRE_EQUAL(total, 0);
    });
}

SEASTAR_TEST_CASE(test_async_generator_throws_from_consumer) {
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
    co_await count_to(42).then_wrapped([&total] (auto f) {
        BOOST_REQUIRE(f.failed());
        BOOST_REQUIRE_THROW(std::rethrow_exception(f.get_exception()), std::invalid_argument);
        BOOST_REQUIRE_EQUAL(total, 0);
    });
}
