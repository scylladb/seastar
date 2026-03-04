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
 * Copyright (C) 2022 Kefu Chai ( tchaikov@gmail.com )
 */

#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/coroutine/generator.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/util/bool_class.hh>
#include <seastar/util/later.hh>
#include <string>
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

    while (auto actual_fib = co_await actual_fibs()) {
        BOOST_REQUIRE(expected_fib != std::end(expected_fibs));
        BOOST_REQUIRE_EQUAL(*actual_fib, *expected_fib);
        ++expected_fib;
    }
    BOOST_REQUIRE(expected_fib == std::end(expected_fibs));
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
    auto actual_fib = co_await fib();
    BOOST_REQUIRE(actual_fib.has_value());
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
    size_t idx = 0;
    while (auto actual = co_await actual_quoted()) {
        BOOST_REQUIRE(idx < expected_quoted.size());
        // generator<std::string_view, std::string> returns std::string_view as a value
        BOOST_REQUIRE_EQUAL(*actual, expected_quoted[idx]);
        ++idx;
    }
    BOOST_REQUIRE_EQUAL(idx, expected_quoted.size());
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
    size_t idx = 0;
    while (auto actual = co_await actual_strings()) {
        BOOST_REQUIRE(idx < expected_strings.size());
        // generator<std::string> returns std::string&& wrapped in reference_wrapper
        BOOST_REQUIRE_EQUAL(actual->get(), expected_strings[idx]);
        ++idx;
    }
    BOOST_REQUIRE_EQUAL(idx, expected_strings.size());
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
        for (unsigned i = 0; i < 2 * n; i++) {
            co_await count();
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
        for (unsigned i = 0; i < n; i++) {
            if (i == n / 2) {
                throw std::invalid_argument("Eureka from consumer!");
            }
            co_await count();
        }
    };
    auto f = co_await coroutine::as_future(count_to(42));
    BOOST_REQUIRE(f.failed());
    BOOST_REQUIRE_THROW(std::rethrow_exception(f.get_exception()), std::invalid_argument);
    BOOST_REQUIRE_EQUAL(total, 0);
}

SEASTAR_TEST_CASE(test_batch_generator_empty_sequence) {
    using int_gen = coroutine::experimental::generator<int, int, std::vector<int>>;
    auto seq = std::invoke([]() -> int_gen {
        co_return;
    });
    while (auto val = co_await seq()) {
        BOOST_FAIL("found element in an empty sequence");
    }
}

coroutine::experimental::generator<int, int, std::vector<int>>
async_fibonacci_sequence_batch(unsigned count, unsigned batch_size, do_suspend suspend) {
    auto a = 0, b = 1;
    std::vector<int> batch;
    for (unsigned i = 0; i < count; ++i) {
        if (std::numeric_limits<decltype(a)>::max() - a < b) {
            throw std::out_of_range(
                fmt::format("fibonacci[{}] is greater than the largest value of int", i));
        }
        if (suspend) {
            co_await yield();
        }
        int next = std::exchange(a, std::exchange(b, a + b));
        batch.push_back(next);
        if (batch.size() == batch_size) {
            co_yield std::exchange(batch, {});
        }
    }
    if (!batch.empty()) {
        co_yield std::move(batch);
    }
}

seastar::future<>
verify_fib_drained(coroutine::experimental::generator<int, int, std::vector<int>> actual_fibs, unsigned count) {
    auto expected_fibs = sync_fibonacci_sequence(count);
    auto expected_fib = std::begin(expected_fibs);

    while (auto actual_fib = co_await actual_fibs()) {
        BOOST_REQUIRE(expected_fib != std::end(expected_fibs));
        BOOST_REQUIRE_EQUAL(*actual_fib, *expected_fib);
        ++expected_fib;
    }
    BOOST_REQUIRE(expected_fib == std::end(expected_fibs));
}

SEASTAR_TEST_CASE(test_batch_generator_drained_with_suspend) {
    constexpr unsigned count = 4;
    constexpr unsigned batch_size = 2;
    return verify_fib_drained(async_fibonacci_sequence_batch(count, batch_size, do_suspend::yes),
                              count);
}

SEASTAR_TEST_CASE(test_batch_generator_drained_without_suspend) {
    constexpr int count = 4;
    constexpr int batch_size = 2;
    return verify_fib_drained(async_fibonacci_sequence_batch(count, batch_size, do_suspend::no),
                              count);
}

seastar::future<> test_batch_generator_not_drained(do_suspend suspend) {
    auto fib = async_fibonacci_sequence_batch(42, 12, suspend);
    auto actual_fib = co_await fib();
    BOOST_REQUIRE(actual_fib.has_value());
    BOOST_REQUIRE_EQUAL(*actual_fib, 0);
}

SEASTAR_TEST_CASE(test_batch_generator_not_drained_with_suspend) {
    return test_batch_generator_not_drained(do_suspend::yes);
}

SEASTAR_TEST_CASE(test_batch_generator_not_drained_without_suspend) {
    return test_batch_generator_not_drained(do_suspend::no);
}

SEASTAR_TEST_CASE(test_batch_generator_move_away) {
    struct move_only {
        int value;
        move_only(int value)
            : value{value}
        {}
        move_only(const move_only&) = delete;
        move_only& operator=(const move_only&) = delete;
        move_only(move_only&&) noexcept = default;
        move_only& operator=(move_only&&) noexcept = default;
    };

    using batch_type = std::vector<move_only>;
    // Changed from move_only&& to move_only& because buffer elements are lvalues
    using move_only_gen = coroutine::experimental::generator<move_only&, move_only, batch_type>;

    constexpr int count = 4;
    constexpr unsigned batch_size = 2;
    auto numbers = std::invoke([]() -> move_only_gen {
        batch_type batch;
        for (int i = 0; i < count; i++) {
            batch.push_back(i);
            if (batch.size() == batch_size) {
                co_yield std::exchange(batch, {});
            }
        }
        if (!batch.empty()) {
            co_yield std::move(batch);
        }
    });

    int expected_n = 0;
    while (auto n = co_await numbers()) {
        BOOST_REQUIRE_EQUAL(n->get().value, expected_n++);
    }
}

SEASTAR_TEST_CASE(test_batch_generator_convertible) {
    struct convertible {
        const std::string value;
        convertible(std::string&& value)
            : value{std::move(value)}
        {}
        explicit operator int() const {
            return std::stoi(value);
        }
    };

    using batch_type = std::vector<convertible>;
    using move_only_gen = coroutine::experimental::generator<int, convertible, batch_type>;

    constexpr int count = 4;
    constexpr unsigned batch_size = 2;
    auto numbers = std::invoke([]() -> move_only_gen {
        batch_type batch;
        for (int i = 0; i < count; i++) {
            batch.push_back(fmt::to_string(i));
            if (batch.size() == batch_size) {
                co_yield std::exchange(batch, {});
            }
        }
        if (!batch.empty()) {
            co_yield std::move(batch);
        }
    });

    int expected_n = 0;
    while (auto n = co_await numbers()) {
        BOOST_REQUIRE_EQUAL(*n, expected_n++);
    }
}

// ADL test helper (must be at namespace scope for ADL to work)
namespace test_adl_ns {
    struct adl_container {
        using value_type = int;
        std::vector<int> data;
        size_t max_elements = 5;

        size_t size() const { return data.size(); }
        size_t capacity() const { return data.capacity(); }
        void push_back(int v) { data.push_back(v); }
        void clear() { data.clear(); }
        int& operator[](size_t idx) { return data[idx]; }
    };

    // ADL free function - priority 2 (found via ADL)
    bool can_push_more(const adl_container& c) {
        return c.data.size() < c.max_elements;
    }
}

// Test can_push_more() CPO with all three customization points
SEASTAR_TEST_CASE(test_can_push_more_cpo) {
    using namespace coroutine::experimental::internal::buffered;

    // Test 1: Default implementation (size < capacity)
    {
        std::vector<int> vec;
        vec.reserve(10);
        BOOST_REQUIRE(can_push_more(vec));  // size=0, capacity=10
        vec.push_back(1);
        vec.push_back(2);
        BOOST_REQUIRE(can_push_more(vec));  // size=2, capacity=10
        vec.resize(10);
        BOOST_REQUIRE(!can_push_more(vec));  // size=10, capacity=10
    }

    // Test 2: Member function customization
    {
        struct member_func_container {
            using value_type [[maybe_unused]] = int;
            std::vector<int> data;
            size_t memory_limit = 100;
            size_t memory_used = 0;

            size_t size() const { return data.size(); }
            size_t capacity() const { return data.capacity(); }
            void push_back(int v) {
                data.push_back(v);
                memory_used += sizeof(int);
            }
            void clear() {
                data.clear();
                memory_used = 0;
            }
            int& operator[](size_t idx) { return data[idx]; }

            // Member function customization - priority 1
            bool can_push_more() const {
                return memory_used < memory_limit;
            }
        };

        member_func_container cont;
        cont.data.reserve(1000);  // Large capacity
        BOOST_REQUIRE(can_push_more(cont));  // memory_used=0 < memory_limit=100

        // Fill with data
        for (size_t i = 0; i < 20; ++i) {
            cont.push_back(i);
        }
        BOOST_REQUIRE(cont.memory_used == 20 * sizeof(int));  // 80 bytes
        BOOST_REQUIRE(can_push_more(cont));  // 80 < 100

        cont.push_back(42);
        cont.push_back(43);
        cont.push_back(44);
        BOOST_REQUIRE(cont.memory_used == 23 * sizeof(int));  // 92 bytes
        BOOST_REQUIRE(can_push_more(cont));  // 92 < 100

        cont.push_back(45);
        cont.push_back(46);
        BOOST_REQUIRE(cont.memory_used == 25 * sizeof(int));  // 100 bytes
        BOOST_REQUIRE(!can_push_more(cont));  // 100 >= 100
    }

    // Test 3: ADL free function customization
    {
        test_adl_ns::adl_container cont;
        cont.data.reserve(100);  // Large capacity
        BOOST_REQUIRE(can_push_more(cont));  // size=0 < max_elements=5

        cont.push_back(1);
        cont.push_back(2);
        cont.push_back(3);
        BOOST_REQUIRE(can_push_more(cont));  // size=3 < max_elements=5

        cont.push_back(4);
        BOOST_REQUIRE(can_push_more(cont));  // size=4 < max_elements=5

        cont.push_back(5);
        BOOST_REQUIRE(!can_push_more(cont));  // size=5 >= max_elements=5
    }

    co_return;
}
