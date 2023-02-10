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
 * Copyright (C) 2017 ScyllaDB Ltd.
 */


#define BOOST_TEST_MODULE core

#include <boost/test/unit_test.hpp>
#include <seastar/util/noncopyable_function.hh>

using namespace seastar;

BOOST_AUTO_TEST_CASE(basic_tests) {
    struct s {
        int f1(int x) const { return x + 1; }
        int f2(int x) { return x + 2; }
        static int f3(int x) { return x + 3; }
        int operator()(int x) const { return x + 4; }
    };
    s obj, obj2;
    auto fn1 = noncopyable_function<int (const s*, int)>(&s::f1);
    auto fn2 = noncopyable_function<int (s*, int)>(&s::f2);
    auto fn3 = noncopyable_function<int (int)>(&s::f3);
    auto fn4 = noncopyable_function<int (int)>(std::move(obj2));
    BOOST_REQUIRE_EQUAL(fn1(&obj, 1), 2);
    BOOST_REQUIRE_EQUAL(fn2(&obj, 1), 3);
    BOOST_REQUIRE_EQUAL(fn3(1), 4);
    BOOST_REQUIRE_EQUAL(fn4(1), 5);
}

template <size_t Extra>
struct payload {
    static unsigned live;
    char extra[Extra];
    std::unique_ptr<int> v;
    payload(int x) : v(std::make_unique<int>(x)) { ++live; }
    payload(payload&& x) noexcept : v(std::move(x.v)) { ++live; }
    void operator=(payload&&) = delete;
    ~payload() { --live; }
    int operator()() const { return *v; }
};

template <size_t Extra>
unsigned payload<Extra>::live;

template <size_t Extra>
void do_move_tests() {
    using payload = ::payload<Extra>;
    auto f1 = noncopyable_function<int ()>(payload(3));
    BOOST_REQUIRE_EQUAL(payload::live, 1u);
    BOOST_REQUIRE_EQUAL(f1(), 3);
    auto f2 = noncopyable_function<int ()>();
    BOOST_CHECK_THROW(f2(), std::bad_function_call);
    f2 = std::move(f1);
    BOOST_CHECK_THROW(f1(), std::bad_function_call);
    BOOST_REQUIRE_EQUAL(f2(), 3);
    BOOST_REQUIRE_EQUAL(payload::live, 1u);
    f2 = {};
    BOOST_REQUIRE_EQUAL(payload::live, 0u);
    BOOST_CHECK_THROW(f2(), std::bad_function_call);
}

BOOST_AUTO_TEST_CASE(small_move_tests) {
    do_move_tests<1>();
}

BOOST_AUTO_TEST_CASE(large_move_tests) {
    do_move_tests<1000>();
}

