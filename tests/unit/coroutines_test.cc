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

#include <seastar/core/coroutine.hh>
#include <seastar/core/future-util.hh>
#include <seastar/testing/test_case.hh>

using namespace seastar;

namespace {

future<int> old_fashioned_continuations() {
    return later().then([] {
        return 42;
    });
}

future<int> simple_coroutine() {
    co_await later();
    co_return 53;
}

future<int> ready_coroutine() {
    co_return 64;
}

future<int, double> tuple_coroutine() {
    co_return std::tuple(1, 2.);
}

future<int> failing_coroutine() {
    co_await later();
    throw 42;
}

}

SEASTAR_TEST_CASE(test_simple_coroutines) {
    BOOST_REQUIRE_EQUAL(co_await old_fashioned_continuations(), 42);
    BOOST_REQUIRE_EQUAL(co_await simple_coroutine(), 53);
    BOOST_REQUIRE_EQUAL(ready_coroutine().get0(), 64);
    BOOST_REQUIRE(co_await tuple_coroutine() == std::tuple(1, 2.));
    BOOST_REQUIRE_EXCEPTION((void)co_await failing_coroutine(), int, [] (auto v) { return v == 42; });
}

SEASTAR_TEST_CASE(test_abandond_coroutine) {
    compat::optional<future<int>> f;
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

    auto p1 = promise<>();
    auto p2 = promise<>();

    auto p1b = promise<>();
    auto p2b = promise<>();
    auto f1 = p1b.get_future();
    auto f2 = p2b.get_future();

    BOOST_REQUIRE(current_scheduling_group() == default_scheduling_group());
    auto f_ret = with_scheduling_group(other_sg,
            [other_sg] (future<> f1, future<> f2, promise<> p1, promise<> p2) -> future<int> {
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
}
