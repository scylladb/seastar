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

#include <exception>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

#include <seastar/core/gate.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/abort_on_expiry.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/later.hh>

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

SEASTAR_TEST_CASE(test_sleep_abortable_no_abort) {
    auto as = std::make_unique<abort_source>();

    // Check that the sleep completes as usual if the
    // abort source doesn't fire.
    auto f = sleep_abortable<manual_clock>(100s, *as);
    manual_clock::advance(99s);
    BOOST_REQUIRE(!f.available());
    manual_clock::advance(101s);
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

SEASTAR_THREAD_TEST_CASE(test_destroy_with_moved_subscriptions) {
    auto as = std::make_unique<abort_source>();
    int aborted = 0;
    auto sub1 = as->subscribe([&] () noexcept { ++aborted; });
    auto sub2 = std::move(sub1);
    optimized_optional<abort_source::subscription> sub3;
    sub3 = std::move(sub2);
    auto sub4 = as->subscribe([&] () noexcept { ++aborted; });
    sub4 = std::move(sub3);
    as.reset();
    BOOST_REQUIRE_EQUAL(aborted, 0);
}

SEASTAR_THREAD_TEST_CASE(test_request_abort_twice) {
    abort_source as;
    as.request_abort_ex(std::runtime_error(""));
    as.request_abort();
    BOOST_REQUIRE_THROW(as.check(), std::runtime_error);
}

SEASTAR_THREAD_TEST_CASE(test_on_abort_call_after_abort) {
    std::exception_ptr signalled_ex;
    auto as = abort_source();
    auto sub = as.subscribe([&] (const std::optional<std::exception_ptr>& ex) noexcept {
        BOOST_REQUIRE(!signalled_ex);
        signalled_ex = *ex;
    });
    BOOST_REQUIRE_EQUAL(bool(sub), true);
    BOOST_REQUIRE(signalled_ex == nullptr);

    // on_abort should trigger the subscribed callback
    as.request_abort_ex(std::make_exception_ptr(std::runtime_error("signaled")));
    BOOST_REQUIRE_EQUAL(bool(sub), false);
    BOOST_REQUIRE(signalled_ex != nullptr);
    BOOST_REQUIRE_THROW(std::rethrow_exception(signalled_ex), std::runtime_error);

    // on_abort is single-shot
    signalled_ex = nullptr;
    sub->on_abort(std::make_exception_ptr(std::runtime_error("signaled")));
    BOOST_REQUIRE(signalled_ex == nullptr);
}

SEASTAR_THREAD_TEST_CASE(test_on_abort_call_before_abort) {
    std::exception_ptr signalled_ex;
    auto as = abort_source();
    auto sub = as.subscribe([&] (const std::optional<std::exception_ptr>& ex) noexcept {
        BOOST_REQUIRE(!signalled_ex);
        signalled_ex = *ex;
    });
    BOOST_REQUIRE_EQUAL(bool(sub), true);
    BOOST_REQUIRE(signalled_ex == nullptr);

    // on_abort should trigger the subscribed callback
    sub->on_abort(std::make_exception_ptr(std::runtime_error("signaled")));
    BOOST_REQUIRE_EQUAL(bool(sub), false);
    BOOST_REQUIRE(signalled_ex != nullptr);
    BOOST_REQUIRE_THROW(std::rethrow_exception(signalled_ex), std::runtime_error);

    // subscription is single-shot
    signalled_ex = nullptr;
    as.request_abort_ex(std::make_exception_ptr(std::runtime_error("signaled")));
    BOOST_REQUIRE(signalled_ex == nullptr);
}

SEASTAR_THREAD_TEST_CASE(test_subscribe_aborted_source) {
    std::exception_ptr signalled_ex;
    auto as = abort_source();
    as.request_abort();
    auto sub = as.subscribe([&] (const std::optional<std::exception_ptr>& ex) noexcept {
        BOOST_REQUIRE(!signalled_ex);
        signalled_ex = *ex;
    });

    // subscription is expected to evaluate to false
    // if abort_source was already aborted
    BOOST_REQUIRE_EQUAL(bool(sub), false);
    BOOST_REQUIRE(signalled_ex == nullptr);

    // on_abort should trigger the subscribed callback
    // if abort_source was already aborted
    sub->on_abort(std::make_exception_ptr(std::runtime_error("signaled")));
    BOOST_REQUIRE(signalled_ex != nullptr);
    BOOST_REQUIRE_THROW(std::rethrow_exception(signalled_ex), std::runtime_error);

    // on_abort is single-shot
    signalled_ex = nullptr;
    sub->on_abort(std::make_exception_ptr(std::runtime_error("signaled")));
    BOOST_REQUIRE(signalled_ex == nullptr);
}

SEASTAR_THREAD_TEST_CASE(test_subscription_callback_lifetime) {
    // The subscription callback function needs to be destroyed
    // only when the subscription is destroyed.
    bool callback_destroyed = false;
    int callback_called = 0;
    auto when_destroyed = deferred_action([&callback_destroyed] () noexcept { callback_destroyed = true; });
    auto as = abort_source();
    auto sub = std::make_unique<optimized_optional<abort_source::subscription>>(as.subscribe([&, when_destroyed = std::move(when_destroyed)] (const std::optional<std::exception_ptr>& ex) noexcept {
        callback_called++;
    }));
    BOOST_REQUIRE_EQUAL(bool(sub), true);
    BOOST_REQUIRE_EQUAL(bool(*sub), true);
    BOOST_REQUIRE_EQUAL(callback_destroyed, false);
    BOOST_REQUIRE_EQUAL(callback_called, 0);

    // on_abort should trigger the subscribed callback
    as.request_abort_ex(std::make_exception_ptr(std::runtime_error("signaled")));
    BOOST_REQUIRE_EQUAL(bool(*sub), false);
    BOOST_REQUIRE_EQUAL(callback_destroyed, false);
    BOOST_REQUIRE_EQUAL(callback_called, 1);

    // on_abort is single-shot
    (*sub)->on_abort(std::make_exception_ptr(std::runtime_error("signaled")));
    BOOST_REQUIRE_EQUAL(callback_destroyed, false);
    BOOST_REQUIRE_EQUAL(callback_called, 1);

    sub.reset();
    BOOST_REQUIRE_EQUAL(callback_destroyed, true);
    BOOST_REQUIRE_EQUAL(callback_called, 1);
}

SEASTAR_THREAD_TEST_CASE(test_abort_on_expiry) {
    auto abort = abort_on_expiry<manual_clock>(manual_clock::now() + 1s);
    std::exception_ptr ex;
    int called = 0;
    auto sub = abort.abort_source().subscribe([&] (const std::optional<std::exception_ptr>& ex_opt) noexcept {
        called++;
        if (ex_opt) {
            ex = *ex_opt;
        }
    });
    BOOST_REQUIRE(!abort.abort_source().abort_requested());
    BOOST_REQUIRE(!called);
    manual_clock::advance(1s);
    yield().get();
    BOOST_REQUIRE(abort.abort_source().abort_requested());
    BOOST_REQUIRE_EQUAL(called, 1);
    BOOST_REQUIRE(ex != nullptr);
    BOOST_REQUIRE_THROW(std::rethrow_exception(ex), timed_out_error);
}
