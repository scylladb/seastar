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

#include <exception>

#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

#include <seastar/core/gate.hh>
#include <seastar/util/closeable.hh>

using namespace seastar;

class expected_exception : public std::runtime_error {
public:
    expected_exception() : runtime_error("expected") {}
};

SEASTAR_TEST_CASE(deferred_close_test) {
  return do_with(gate(), 0, 42, [] (gate& g, int& count, int& expected) {
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
  });
}

SEASTAR_TEST_CASE(close_now_test) {
  return do_with(gate(), 0, 42, [] (gate& g, int& count, int& expected) {
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
  });
}

SEASTAR_TEST_CASE(with_closeable_test) {
    return do_with(0, 42, [] (int& count, int& expected) {
        return with_closeable(gate(), [&] (gate& g) {
            for (auto i = 0; i < expected; i++) {
                (void)with_gate(g, [&count] {
                    ++count;
                });
            }
            return 17;
        }).then([&] (int res) {
            // res should be returned by the function called
            // by with_closeable.
            BOOST_REQUIRE_EQUAL(res, 17);
            // closing the gate should wait for
            // all background continuations to complete
            BOOST_REQUIRE_EQUAL(count, expected);
        });
    });
}

SEASTAR_TEST_CASE(with_closeable_exception_test) {
    return do_with(0, 42, [] (int& count, int& expected) {
        return with_closeable(gate(), [&] (gate& g) {
            for (auto i = 0; i < expected; i++) {
                (void)with_gate(g, [&count] {
                    ++count;
                });
            }
            throw expected_exception();
        }).handle_exception_type([&] (const expected_exception&) {
            // closing the gate should also happen when func throws,
            // waiting for all background continuations to complete
            BOOST_REQUIRE_EQUAL(count, expected);
        });
    });
}

namespace {

class count_stops {
    int _count = -1;
    int* _ptr = nullptr;
public:
    count_stops(int* ptr = nullptr) noexcept
        : _ptr(ptr ? ptr : &_count)
    {
        *_ptr = 0;
    }

    count_stops(count_stops&& o) noexcept {
        std::exchange(_count, o._count);
        if (o._ptr == &o._count) {
            _ptr = &_count;
        } else {
            std::exchange(_ptr, o._ptr);
        }
    }

    future<> stop() {
        ++*_ptr;
        return make_ready_future<>();
    }

    int stopped() const noexcept {
        return *_ptr;
    }
};

} // anonymous namespace

SEASTAR_TEST_CASE(deferred_stop_test) {
  return do_with(count_stops(), [] (count_stops& cs) {
    return async([&] {
        auto stop_counting = deferred_stop(cs);
    }).then([&] {
        // cs.stop() should be called when stop_counting is destroyed
        BOOST_REQUIRE_EQUAL(cs.stopped(), 1);
    });
  });
}

SEASTAR_TEST_CASE(stop_now_test) {
  return do_with(count_stops(), [] (count_stops& cs) {
    return async([&] {
        auto stop_counting = deferred_stop(cs);

        stop_counting.stop_now();
        // cs.stop() should not be called again
        // when stop_counting is destroyed
        BOOST_REQUIRE_EQUAL(cs.stopped(), 1);
    }).then([&] {
        // cs.stop() should be called exactly once
        BOOST_REQUIRE_EQUAL(cs.stopped(), 1);
    });
  });
}

SEASTAR_TEST_CASE(with_stoppable_test) {
    return do_with(0, [] (int& stopped) {
        return with_stoppable(count_stops(&stopped), [] (count_stops& cs) {
            return 17;
        }).then([&] (int res) {
            // res should be returned by the function called
            // by with_closeable.
            BOOST_REQUIRE_EQUAL(res, 17);
            // cs.stop() should be called before count_stops is destroyed
            BOOST_REQUIRE_EQUAL(stopped, 1);
        });
    });
}

SEASTAR_TEST_CASE(with_stoppable_exception_test) {
    return do_with(0, [] (int& stopped) {
        return with_stoppable(count_stops(&stopped), [] (count_stops& cs) {
            throw expected_exception();
        }).handle_exception_type([&] (const expected_exception&) {
            // cs.stop() should be called before count_stops is destroyed
            // also when func throws
            BOOST_REQUIRE_EQUAL(stopped, 1);
        });
    });
}
