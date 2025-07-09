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

// These groups and supergroups are shared between test cases
std::vector<seastar::scheduling_supergroup> sgroups;
std::vector<seastar::scheduling_group> groups;

static future<> maybe_create_groups() {
    if (sgroups.size() > 0) {
        co_return;
    }

    fmt::print("Creating supergroups\n");
    sgroups.resize(2);
    sgroups[0] = co_await seastar::create_scheduling_supergroup(100);
    sgroups[1] = co_await seastar::create_scheduling_supergroup(100);

    fmt::print("Creating groups\n");
    groups.resize(9);
    groups[0] = co_await seastar::create_scheduling_group("g1", 100);
    groups[1] = co_await seastar::create_scheduling_group("g2", 100);
    groups[2] = co_await seastar::create_scheduling_group("g3", 100);
    groups[3] = co_await seastar::create_scheduling_group("g11", "g11", 100, sgroups[0]);
    groups[4] = co_await seastar::create_scheduling_group("g21", "g21", 100, sgroups[0]);
    groups[5] = co_await seastar::create_scheduling_group("g31", "g31", 100, sgroups[0]);
    groups[6] = co_await seastar::create_scheduling_group("g12", "g12", 100, sgroups[1]);
    groups[7] = co_await seastar::create_scheduling_group("g22", "g22", 100, sgroups[1]);
    groups[8] = co_await seastar::create_scheduling_group("g32", "g32", 100, sgroups[1]);
}

SEASTAR_TEST_CASE(test_basic_ops) {
    co_await maybe_create_groups();
    auto ssg = internal::scheduling_supergroup_for(groups[0]);
    BOOST_CHECK(ssg == scheduling_supergroup());
    ssg = internal::scheduling_supergroup_for(groups[6]);
    BOOST_CHECK(!ssg.is_root());
    BOOST_CHECK(ssg.index() == 1);
    BOOST_CHECK(ssg == sgroups[1]);
}
