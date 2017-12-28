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
 * Copyright (C) 2017 ScyllaDB
 */

#include "tests/test-utils.hh"

#include "core/gate.hh"
#include "core/sleep.hh"

using namespace seastar;
using namespace std::chrono_literals;

SEASTAR_TEST_CASE(test_abort_source_notifies_subscriber) {
    bool signalled = false;
    auto as = abort_source();
    auto st_opt = as.subscribe([&signalled] {
        signalled = true;
    });
    BOOST_REQUIRE_EQUAL(true, bool(st_opt));
    as.request_abort();
    BOOST_REQUIRE_EQUAL(true, signalled);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_abort_source_subscription_unregister) {
    bool signalled = false;
    auto as = abort_source();
    auto st_opt = as.subscribe([&signalled] {
        signalled = true;
    });
    BOOST_REQUIRE_EQUAL(true, bool(st_opt));
    st_opt = { };
    as.request_abort();
    BOOST_REQUIRE_EQUAL(false, signalled);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_abort_source_rejects_subscription) {
    auto as = abort_source();
    as.request_abort();
    auto st_opt = as.subscribe([] { });
    BOOST_REQUIRE_EQUAL(false, bool(st_opt));
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_sleep_abortable) {
    auto as = std::make_unique<abort_source>();
    auto f = sleep_abortable(100s, *as).then_wrapped([] (auto&& f) {
        try {
            f.get();
            BOOST_FAIL("should have failed");
        } catch (const sleep_aborted& e) {
            // expected
        } catch (...) {
            BOOST_FAIL("unexpected exception");
        }
    });
    as->request_abort();
    return f.finally([as = std::move(as)] { });
}