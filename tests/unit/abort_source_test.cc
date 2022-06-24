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

#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

#include <seastar/core/gate.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/do_with.hh>

using namespace seastar;
using namespace std::chrono_literals;

SEASTAR_TEST_CASE(test_abort_source_notifies_subscriber) {
    bool signalled = false;
    auto as = abort_source();
    auto st_opt = as.subscribe([&signalled] () noexcept {
        signalled = true;
    });
    BOOST_REQUIRE_EQUAL(true, bool(st_opt));
    as.request_abort();
    BOOST_REQUIRE_EQUAL(true, signalled);
    BOOST_REQUIRE_EQUAL(false, bool(st_opt));
    BOOST_REQUIRE_THROW(as.check(), abort_requested_exception);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_abort_source_subscription_unregister) {
    bool signalled = false;
    auto as = abort_source();
    auto st_opt = as.subscribe([&signalled] () noexcept {
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
    auto st_opt = as.subscribe([] () noexcept { });
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

// Verify that negative sleep does not sleep forever. It should not sleep
// at all.
SEASTAR_TEST_CASE(test_negative_sleep_abortable) {
    return do_with(abort_source(), [] (abort_source& as) {
        return sleep_abortable(-10s, as);
    });
}

SEASTAR_TEST_CASE(test_request_abort_with_exception) {
    abort_source as;
    optimized_optional<abort_source::subscription> st_opt;
    std::optional<std::exception_ptr> aborted_ex;
    auto expected_message = "expected";

    auto make_abort_source = [&] () {
        as = abort_source();
        st_opt = as.subscribe([&aborted_ex] (const std::optional<std::exception_ptr>& opt_ex) noexcept {
            aborted_ex = opt_ex;
        });
        aborted_ex = std::nullopt;
    };

    make_abort_source();
    auto ex = make_exception_ptr(std::runtime_error(expected_message));
    as.request_abort_ex(ex);
    BOOST_REQUIRE(aborted_ex.has_value());
    bool caught_exception = false;
    try {
        std::rethrow_exception(*aborted_ex);
    } catch (const std::runtime_error& e) {
        BOOST_REQUIRE_EQUAL(e.what(), expected_message);
        caught_exception = true;
    }
    BOOST_REQUIRE(caught_exception);
    BOOST_REQUIRE_THROW(as.check(), std::runtime_error);

    make_abort_source();
    as.request_abort_ex(make_exception_ptr(std::runtime_error(expected_message)));
    BOOST_REQUIRE(aborted_ex.has_value());
    caught_exception = false;
    try {
        std::rethrow_exception(*aborted_ex);
    } catch (const std::runtime_error& e) {
        BOOST_REQUIRE_EQUAL(e.what(), expected_message);
        caught_exception = true;
    }
    BOOST_REQUIRE(caught_exception);
    BOOST_REQUIRE_THROW(as.check(), std::runtime_error);


    make_abort_source();
    as.request_abort_ex(std::runtime_error(expected_message));
    BOOST_REQUIRE(aborted_ex.has_value());
    caught_exception = false;
    try {
        std::rethrow_exception(*aborted_ex);
    } catch (const std::runtime_error& e) {
        BOOST_REQUIRE_EQUAL(e.what(), expected_message);
        caught_exception = true;
    }
    BOOST_REQUIRE(caught_exception);
    BOOST_REQUIRE_THROW(as.check(), std::runtime_error);

    return make_ready_future<>();
}

SEASTAR_THREAD_TEST_CASE(test_sleep_abortable_with_exception) {
    abort_source as;
    auto f = sleep_abortable(10s, as);
    auto expected_message = "expected";
    as.request_abort_ex(std::runtime_error(expected_message));

    bool caught_exception = false;
    try {
        f.get();
    } catch (const std::runtime_error& e) {
        BOOST_REQUIRE_EQUAL(e.what(), expected_message);
        caught_exception = true;
    }
    BOOST_REQUIRE(caught_exception);
}
