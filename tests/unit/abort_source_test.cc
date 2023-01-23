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
#include <seastar/testing/random.hh>

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

SEASTAR_THREAD_TEST_CASE(test_abort_source_move) {
    auto as0 = abort_source();
    int aborted = 0;
    auto sub = as0.subscribe([&] () noexcept { ++aborted; });
    auto as1 = std::move(as0);
    as1.request_abort();
    BOOST_REQUIRE(as1.abort_requested());
    BOOST_REQUIRE_EQUAL(aborted, 1);
}

SEASTAR_THREAD_TEST_CASE(test_abort_on_any) {
    std::default_random_engine random_engine(testing::local_random_engine());
    std::uniform_int_distribution<> dist(0, 1);
    std::array<abort_source, 2> ass;
    auto awe = abort_on_any(ass[0], ass[1]);
    auto& as = awe.abort_source();
    BOOST_REQUIRE(!as.abort_requested());

    int aborted = 0;
    std::optional<std::exception_ptr> aborted_ex;
    auto sub = as.subscribe([&] (const std::optional<std::exception_ptr>& opt_ex) noexcept {
        BOOST_REQUIRE(!aborted);
        ++aborted;
        aborted_ex = opt_ex;
    });
    BOOST_REQUIRE(bool(sub));

    int idx = dist(random_engine);
    ass[idx].request_abort_ex(std::runtime_error("expected"));
    BOOST_REQUIRE(as.abort_requested());
    BOOST_REQUIRE_THROW(as.check(), std::runtime_error);

    BOOST_REQUIRE_EQUAL(aborted, 1);
    BOOST_REQUIRE(aborted_ex);
    BOOST_REQUIRE_THROW(std::rethrow_exception(*aborted_ex), std::runtime_error);

    ass[idx ^ 1].request_abort();
    BOOST_REQUIRE(as.abort_requested());
    BOOST_REQUIRE_THROW(as.check(), std::runtime_error);
    BOOST_REQUIRE_EQUAL(aborted, 1);
}

SEASTAR_THREAD_TEST_CASE(test_abort_on_any_three) {
    std::default_random_engine random_engine(testing::local_random_engine());
    std::uniform_int_distribution<> dist(0, 2);
    std::array<abort_source, 3> ass;
    auto awe = abort_on_any(ass[0], ass[1], ass[2]);
    auto& as = awe.abort_source();
    BOOST_REQUIRE(!as.abort_requested());

    int aborted = 0;
    std::optional<std::exception_ptr> aborted_ex;
    auto sub = as.subscribe([&] (const std::optional<std::exception_ptr>& opt_ex) noexcept {
        BOOST_REQUIRE(!aborted);
        ++aborted;
        aborted_ex = opt_ex;
    });
    BOOST_REQUIRE(bool(sub));

    int idx = dist(random_engine);
    ass[idx].request_abort_ex(std::runtime_error("expected"));
    BOOST_REQUIRE(as.abort_requested());
    BOOST_REQUIRE_THROW(as.check(), std::runtime_error);

    BOOST_REQUIRE_EQUAL(aborted, 1);
    BOOST_REQUIRE(aborted_ex);
    BOOST_REQUIRE_THROW(std::rethrow_exception(*aborted_ex), std::runtime_error);

    ass[(idx + 1) % 3].request_abort();
    BOOST_REQUIRE(as.abort_requested());
    BOOST_REQUIRE_THROW(as.check(), std::runtime_error);
    BOOST_REQUIRE_EQUAL(aborted, 1);
}

SEASTAR_THREAD_TEST_CASE(test_abort_on_any_already_aborted) {
    std::default_random_engine random_engine(testing::local_random_engine());
    std::uniform_int_distribution<> dist(0, 1);
    std::array<abort_source, 2> ass;

    int idx = dist(random_engine);
    ass[idx].request_abort_ex(std::runtime_error("expected"));

    auto awe = abort_on_any(ass[0], ass[1]);
    auto& as = awe.abort_source();
    BOOST_REQUIRE(as.abort_requested());
    BOOST_REQUIRE_THROW(as.check(), std::runtime_error);

    auto sub = as.subscribe([&] (const std::optional<std::exception_ptr>& opt_ex) noexcept {
    });
    BOOST_REQUIRE(!sub);
}

SEASTAR_THREAD_TEST_CASE(test_abort_on_any_move) {
    std::default_random_engine random_engine(testing::local_random_engine());
    std::uniform_int_distribution<> dist(0, 1);
    std::array<abort_source, 2> ass;
    auto awe0 = abort_on_any(ass[0], ass[1]);

    int aborted = 0;
    std::optional<std::exception_ptr> aborted_ex;
    auto sub = awe0.abort_source().subscribe([&] (const std::optional<std::exception_ptr>& opt_ex) noexcept {
        ++aborted;
        aborted_ex = opt_ex;
    });
    BOOST_REQUIRE(bool(sub));

    auto awe1 = std::move(awe0);
    auto& as = awe1.abort_source();

    int idx = dist(random_engine);
    ass[idx].request_abort_ex(std::runtime_error("expected"));
    BOOST_REQUIRE(as.abort_requested());
    BOOST_REQUIRE_THROW(as.check(), std::runtime_error);
    BOOST_REQUIRE_EQUAL(aborted, 1);
    BOOST_REQUIRE(aborted_ex);
    BOOST_REQUIRE_THROW(std::rethrow_exception(*aborted_ex), std::runtime_error);
}

SEASTAR_THREAD_TEST_CASE(test_abort_on_all) {
    std::default_random_engine random_engine(testing::local_random_engine());
    std::uniform_int_distribution<> dist(0, 1);
    std::array<abort_source, 2> ass;
    auto awb = abort_on_all(ass[0], ass[1]);
    auto& as = awb.abort_source();
    BOOST_REQUIRE(!as.abort_requested());

    int aborted = 0;
    std::optional<std::exception_ptr> aborted_ex;
    auto sub = as.subscribe([&] (const std::optional<std::exception_ptr>& opt_ex) noexcept {
        ++aborted;
        aborted_ex = opt_ex.value_or(std::make_exception_ptr(abort_requested_exception()));
    });
    BOOST_REQUIRE(bool(sub));

    int idx = dist(random_engine);
    ass[idx].request_abort();
    BOOST_REQUIRE(!as.abort_requested());
    BOOST_REQUIRE_NO_THROW(as.check());

    ass[idx ^ 1].request_abort();
    BOOST_REQUIRE_EQUAL(aborted, 1);
    BOOST_REQUIRE(aborted_ex);
    BOOST_REQUIRE_THROW(std::rethrow_exception(*aborted_ex), abort_requested_exception);
    BOOST_REQUIRE(as.abort_requested());
    BOOST_REQUIRE_THROW(as.check(), abort_requested_exception);
}

SEASTAR_THREAD_TEST_CASE(test_abort_on_all_three) {
    std::default_random_engine random_engine(testing::local_random_engine());
    std::uniform_int_distribution<> dist(0, 2);
    std::array<abort_source, 3> ass;
    auto awb = abort_on_all(ass[0], ass[1], ass[2]);
    auto& as = awb.abort_source();
    BOOST_REQUIRE(!as.abort_requested());

    int aborted = 0;
    std::optional<std::exception_ptr> aborted_ex;
    auto sub = as.subscribe([&] (const std::optional<std::exception_ptr>& opt_ex) noexcept {
        ++aborted;
        aborted_ex = opt_ex.value_or(std::make_exception_ptr(abort_requested_exception()));
    });
    BOOST_REQUIRE(bool(sub));

    int idx = dist(random_engine);
    ass[idx].request_abort();
    BOOST_REQUIRE(!as.abort_requested());
    BOOST_REQUIRE_NO_THROW(as.check());
    BOOST_REQUIRE_EQUAL(aborted, 0);

    ass[++idx % 3].request_abort();
    BOOST_REQUIRE_EQUAL(aborted, 0);
    BOOST_REQUIRE(!as.abort_requested());
    BOOST_REQUIRE_NO_THROW(as.check());

    ass[++idx % 3].request_abort();
    BOOST_REQUIRE_EQUAL(aborted, 1);
    BOOST_REQUIRE(aborted_ex);
    BOOST_REQUIRE_THROW(std::rethrow_exception(*aborted_ex), abort_requested_exception);
    BOOST_REQUIRE(as.abort_requested());
    BOOST_REQUIRE_THROW(as.check(), abort_requested_exception);
}

SEASTAR_THREAD_TEST_CASE(test_abort_on_all_already_aborted_once) {
    std::default_random_engine random_engine(testing::local_random_engine());
    std::uniform_int_distribution<> dist(0, 1);
    std::array<abort_source, 2> ass;

    int idx = dist(random_engine);
    ass[idx].request_abort();

    auto awb = abort_on_all(ass[0], ass[1]);
    auto& as = awb.abort_source();
    BOOST_REQUIRE(!as.abort_requested());
    BOOST_REQUIRE_NO_THROW(as.check());

    int aborted = 0;
    std::optional<std::exception_ptr> aborted_ex;
    auto sub = as.subscribe([&] (const std::optional<std::exception_ptr>& opt_ex) noexcept {
        ++aborted;
        aborted_ex = opt_ex.value_or(std::make_exception_ptr(abort_requested_exception()));
    });
    BOOST_REQUIRE(bool(sub));

    ass[idx ^ 1].request_abort();
    BOOST_REQUIRE_EQUAL(aborted, 1);
    BOOST_REQUIRE(aborted_ex);
    BOOST_REQUIRE_THROW(std::rethrow_exception(*aborted_ex), abort_requested_exception);
    BOOST_REQUIRE(as.abort_requested());
    BOOST_REQUIRE_THROW(as.check(), abort_requested_exception);
}

SEASTAR_THREAD_TEST_CASE(test_abort_on_all_already_aborted_twice) {
    std::default_random_engine random_engine(testing::local_random_engine());
    std::uniform_int_distribution<> dist(0, 1);
    std::array<abort_source, 2> ass;

    ass[0].request_abort();
    ass[1].request_abort();

    auto awb = abort_on_all(ass[0], ass[1]);
    auto& as = awb.abort_source();
    BOOST_REQUIRE(as.abort_requested());
    BOOST_REQUIRE_THROW(as.check(), abort_requested_exception);

    auto sub = as.subscribe([&] (const std::optional<std::exception_ptr>& opt_ex) noexcept {
    });
    BOOST_REQUIRE(!sub);
}

SEASTAR_THREAD_TEST_CASE(test_abort_on_all_move) {
    std::default_random_engine random_engine(testing::local_random_engine());
    std::uniform_int_distribution<> dist(0, 1);
    std::array<abort_source, 2> ass;
    auto awb0 = abort_on_all(ass[0], ass[1]);

    int aborted = 0;
    std::optional<std::exception_ptr> aborted_ex;
    auto sub = awb0.abort_source().subscribe([&] (const std::optional<std::exception_ptr>& opt_ex) noexcept {
        ++aborted;
        aborted_ex = opt_ex.value_or(std::make_exception_ptr(abort_requested_exception()));
    });
    BOOST_REQUIRE(bool(sub));

    auto awb1 = std::move(awb0);
    auto& as = awb1.abort_source();

    int idx = dist(random_engine);
    ass[idx].request_abort();
    BOOST_REQUIRE(!as.abort_requested());
    BOOST_REQUIRE_NO_THROW(as.check());
    BOOST_REQUIRE_EQUAL(aborted, 0);
    BOOST_REQUIRE(!aborted_ex);

    ass[idx ^ 1].request_abort();
    BOOST_REQUIRE_EQUAL(aborted, 1);
    BOOST_REQUIRE(aborted_ex);
    BOOST_REQUIRE_THROW(std::rethrow_exception(*aborted_ex), abort_requested_exception);
    BOOST_REQUIRE(as.abort_requested());
    BOOST_REQUIRE_THROW(as.check(), abort_requested_exception);
}
