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
 * Copyright (C) 2025 ScyllaDB Ltd.
 */

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <vector>
#include <chrono>

#include <seastar/core/thread.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/testing/random.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/core/reactor.hh>
#include <seastar/util/later.hh>

using namespace seastar;

struct layout {
    std::vector<seastar::scheduling_supergroup> sgroups;
    std::vector<seastar::scheduling_group> groups;

    static future<layout> create();
    future<> destroy();
};

future<layout> layout::create() {
    layout ret;

    fmt::print("Creating supergroups\n");
    ret.sgroups.resize(2);
    ret.sgroups[0] = co_await seastar::create_scheduling_supergroup(100);
    ret.sgroups[1] = co_await seastar::create_scheduling_supergroup(100);

    fmt::print("Creating groups\n");
    ret.groups.resize(9);
    ret.groups[0] = co_await seastar::create_scheduling_group("g1", 100);
    ret.groups[1] = co_await seastar::create_scheduling_group("g2", 100);
    ret.groups[2] = co_await seastar::create_scheduling_group("g3", 100);
    ret.groups[3] = co_await seastar::create_scheduling_group("g11", "g11", 100, ret.sgroups[0]);
    ret.groups[4] = co_await seastar::create_scheduling_group("g21", "g21", 100, ret.sgroups[0]);
    ret.groups[5] = co_await seastar::create_scheduling_group("g31", "g31", 100, ret.sgroups[0]);
    ret.groups[6] = co_await seastar::create_scheduling_group("g12", "g12", 100, ret.sgroups[1]);
    ret.groups[7] = co_await seastar::create_scheduling_group("g22", "g22", 100, ret.sgroups[1]);
    ret.groups[8] = co_await seastar::create_scheduling_group("g32", "g32", 100, ret.sgroups[1]);

    co_return ret;
}

future<> layout::destroy() {
    for (auto sg : groups) {
        co_await seastar::destroy_scheduling_group(sg);
    }
    for (auto sg : sgroups) {
        co_await seastar::destroy_scheduling_supergroup(sg);
    }
}

SEASTAR_TEST_CASE(test_basic_ops) {
    auto l = co_await layout::create();
    BOOST_CHECK_EXCEPTION(co_await seastar::destroy_scheduling_supergroup(l.sgroups[0]), std::runtime_error, [] (const std::runtime_error& e) {
        return e.what() == sstring("Supergroup is still populated, destroy all subgroups first");
    });
    auto ssg = internal::scheduling_supergroup_for(l.groups[0]);
    BOOST_CHECK(ssg == scheduling_supergroup());
    ssg = internal::scheduling_supergroup_for(l.groups[6]);
    BOOST_CHECK(!ssg.is_root());
    BOOST_CHECK(ssg.index() == 1);
    BOOST_CHECK(ssg == l.sgroups[1]);
    co_await l.destroy();
}
