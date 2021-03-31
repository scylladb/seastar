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
 * Copyright 2021 ScyllaDB
 */


#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

#include <seastar/core/gate.hh>
#include <seastar/util/closeable.hh>

using namespace seastar;

SEASTAR_TEST_CASE(deferred_close_test) {
    int count = 0;
    int expected = 42;
    gate g;

    return async([&] {
        auto close_gate = deferred_close(g);

        for (auto i = 0; i < expected; i++) {
            (void)with_gate(g, [&count] {
                ++count;
            });
        }
    }).then([&] {
        // destroying close_gate should invoke g.close()
        // and wait for all background continuations to complete
        BOOST_REQUIRE(g.is_closed());
        BOOST_REQUIRE_EQUAL(count, expected);
    });
}

SEASTAR_TEST_CASE(close_now_test) {
    int count = 0;
    int expected = 42;
    gate g;

    return async([&] {
        auto close_gate = deferred_close(g);

        for (auto i = 0; i < expected; i++) {
            (void)with_gate(g, [&count] {
                ++count;
            });
        }

        close_gate.close_now();
        BOOST_REQUIRE(g.is_closed());
        BOOST_REQUIRE_EQUAL(count, expected);
        // gate must not be double-closed.
    });
}

namespace {

struct count_stops {
    int stopped = 0;

    future<> stop() {
        ++stopped;
        return make_ready_future<>();
    }
};

} // anonymous namespace

SEASTAR_TEST_CASE(deferred_stop_test) {
    count_stops cs;

    return async([&] {
        auto stop_counting = deferred_stop(cs);
    }).then([&] {
        // cs.stop() should be called when stop_counting is destroyed
        BOOST_REQUIRE_EQUAL(cs.stopped, 1);
    });
}

SEASTAR_TEST_CASE(stop_now_test) {
    count_stops cs;

    return async([&] {
        auto stop_counting = deferred_stop(cs);

        stop_counting.stop_now();
        // cs.stop() should not be called again
        // when stop_counting is destroyed
        BOOST_REQUIRE_EQUAL(cs.stopped, 1);
    }).then([&] {
        // cs.stop() should be called exactly once
        BOOST_REQUIRE_EQUAL(cs.stopped, 1);
    });
}
