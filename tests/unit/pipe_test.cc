/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License"). See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership. You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 * Copyright 2021-present ScyllaDB
 */

#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

#include <seastar/core/pipe.hh>

using namespace seastar;

static_assert(!std::is_default_constructible_v<seastar::pipe_reader<int>>);
static_assert(!std::is_default_constructible_v<seastar::pipe_writer<int>>);

static_assert(std::is_nothrow_move_constructible_v<seastar::pipe_reader<int>>);
static_assert(std::is_nothrow_move_assignable_v<seastar::pipe_reader<int>>);

static_assert(std::is_nothrow_move_constructible_v<seastar::pipe_writer<int>>);
static_assert(std::is_nothrow_move_assignable_v<seastar::pipe_writer<int>>);

SEASTAR_THREAD_TEST_CASE(simple_pipe_test) {
    seastar::pipe<int> p(1);

    auto f0 = p.reader.read();
    BOOST_CHECK(!f0.available());
    p.writer.write(17).get();
    BOOST_REQUIRE_EQUAL(*f0.get0(), 17);

    p.writer.write(42).get();
    auto f2 = p.reader.read();
    BOOST_CHECK(f2.available());
    BOOST_REQUIRE_EQUAL(*f2.get0(), 42);
}
