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
 * Copyright (C) 2024 ScyllaDB.
 */
#include <coroutine>
#include <iostream>

#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>

#include <seastar/testing/test_case.hh>

using namespace seastar;

SEASTAR_TEST_CASE(getgrnam_group_name_does_not_exist_test) {
    // A better approach would be to use a test setup and teardown to create a fake group
    // with members in it and in the teardown delete the fake group.
    std::optional<struct group_details> grp = co_await getgrnam("roo");
    BOOST_REQUIRE(!grp.has_value());
}

SEASTAR_TEST_CASE(getgrnam_group_name_exists_test) {
    std::optional<struct group_details> grp = co_await getgrnam("root");
    BOOST_REQUIRE(grp.has_value());
    BOOST_REQUIRE_EQUAL(grp.value().group_name.c_str(), "root");
}