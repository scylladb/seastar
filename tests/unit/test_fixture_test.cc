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

#include <seastar/testing/test_case.hh>
#include <seastar/testing/test_fixture.hh>

using namespace seastar;
using namespace seastar::testing;

struct AsyncTestFixture {
    bool inited = false;
    bool destroyed = false;

    ~AsyncTestFixture() {
        BOOST_REQUIRE(destroyed);
    }
    future<> setup() {
        inited = true;
        return make_ready_future<>();
    }
    future<> teardown() {
        destroyed = true;
        return make_ready_future<>();
    }
};

SEASTAR_FIXTURE_TEST_CASE(test_single_test_fixture, AsyncTestFixture) {
    BOOST_REQUIRE(inited);
    return make_ready_future<>();
}

struct SyncTestFixture {
    bool inited = false;
    bool destroyed = false;

    ~SyncTestFixture() {
        BOOST_REQUIRE(destroyed);
    }
    void setup() {
        inited = true;
    }
    void teardown() {
        destroyed = true;
    }
};

SEASTAR_FIXTURE_TEST_CASE(test_single_test_fixture_void_ret, SyncTestFixture) {
    BOOST_REQUIRE(inited);
    return make_ready_future<>();
}

SEASTAR_FIXTURE_THREAD_TEST_CASE(test_single_thread_test_fixture_void_ret, SyncTestFixture) {
    BOOST_REQUIRE(inited);
}

// having these thread local subtly verifies that the fixture 
// is run on the proper shard.
static thread_local int num_shared_test_fixts_setup = 0;
static thread_local int num_shared_test_fixts_teardown = 0;
static thread_local std::string shared_test_fixts_string;

struct SharedTestFixture { 
    SharedTestFixture()
    {}
    SharedTestFixture(const std::string& s)
    {
        shared_test_fixts_string = s;
    }
    future<> setup() {
        ++num_shared_test_fixts_setup;
        return make_ready_future<>();
    }
    future<> teardown() {
        ++num_shared_test_fixts_teardown;
        shared_test_fixts_string = {};
        return make_ready_future<>();
    }
};

BOOST_AUTO_TEST_SUITE(shared_fixtures, 
    *async_fixture<SharedTestFixture>()
    *async_fixture<SharedTestFixture>("los lobos")
    *async_fixture(
        [] { ++num_shared_test_fixts_setup; return make_ready_future<>(); },
        [] { ++num_shared_test_fixts_teardown; return make_ready_future<>(); }
    )
)

SEASTAR_TEST_CASE(test_shared_fixture1) {
    BOOST_REQUIRE(num_shared_test_fixts_setup == 3);
    BOOST_REQUIRE(num_shared_test_fixts_teardown == 0);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_shared_fixture2) {
    BOOST_REQUIRE(num_shared_test_fixts_setup == 3);
    BOOST_REQUIRE(num_shared_test_fixts_teardown == 0);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_shared_fixture_init_value) {
    BOOST_REQUIRE(shared_test_fixts_string != "");
    BOOST_TEST_MESSAGE(shared_test_fixts_string);
    return make_ready_future<>();
}

BOOST_AUTO_TEST_SUITE_END()
