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
 * Copyright 2018 ScyllaDB
 */

#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/future-util.hh>
#include <seastar/util/log.hh>
#include <seastar/util/alloc_failure_injector.hh>

using namespace seastar;
using namespace std::chrono_literals;

static seastar::logger testlog("testlog");

SEASTAR_THREAD_TEST_CASE(test_queue_pop_eventually) {
    queue<int> q(100);
    int pushed = 0;
    bool pusher_done = false;
    int popped = 0;
    bool popper_done = false;
    int stop = 0;
    auto test_duration = 1ms;
    auto watchdog_duration = 10s;
    timer stop_timer;
    stop_timer.set_callback([&] {
        testlog.debug("stop_timer: pushed={} pusher_done={} popped={} popper_done={} full={} empty={} stop={}",
                pushed, pusher_done, popped, popper_done,
                q.full(), q.empty(), stop);
        if (!stop++) {
            // First callback is for stopping the test.
            // Second one is a watchdog. Consider the test hung
            // if this callback isn't canceled within 10 seconds.
            // That should be long enough to work in absurdly slow test environments.
            stop_timer.arm(watchdog_duration);
        } else {
            testlog.error("test_queue_pop_eventually is hung: pushed={} pusher_done={} popped={} popper_done={} full={} empty={} stop={}",
                    pushed, pusher_done, popped, popper_done,
                    q.full(), q.empty(), stop);
            abort();
        }
    });
    stop_timer.arm(test_duration);
    auto start = std::chrono::system_clock::now();
    auto pusher = repeat([&] {
        auto&& data = pushed;
        testlog.trace("pusher: full={} empty={} stop={}", q.full(), q.empty(), stop);
        return q.push_eventually(std::move(data)).then([&] {
            pushed++;
            if (stop && !q.empty()) {
                testlog.debug("pusher done");
                pusher_done = true;
                return stop_iteration::yes;
            }
            return stop_iteration::no;
        });
    });
    auto popper = repeat([&] {
        testlog.trace("popper: full={} empty={} stop={}", q.full(), q.empty(), stop);
        if (q.empty()) {
            if (pusher_done) {
                testlog.debug("popper done");
                popper_done = true;
                return make_ready_future<stop_iteration>(true);
            } else if (stop) {
                testlog.debug("popper: full={} empty={} pusher_done={} stop={}", q.full(), q.empty(), pusher_done, stop);
            }
        }
        return q.pop_eventually().then([&] (int&&) {
            popped++;
            return stop_iteration::no;
        });
    });
    pusher.get();
    popper.get();
    auto elapsed = std::chrono::system_clock::now() - start;
    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    stop_timer.cancel();
    BOOST_REQUIRE(q.empty());
    BOOST_REQUIRE(pushed);
    BOOST_REQUIRE(pusher_done);
    BOOST_REQUIRE(popped == pushed);
    BOOST_REQUIRE(popper_done);
    testlog.info("Pushed and popped {} elemements in {}us, {:.3f} elements/us", pushed, elapsed_us, double(pushed) / elapsed_us);
}

#ifdef SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION
SEASTAR_THREAD_TEST_CASE(test_queue_push_eventually_exception) {
    int i = 0;
    queue<int> q(42);
    int intercepted = 0;

    memory::with_allocation_failures([&] {
        BOOST_REQUIRE_NO_THROW(q.push_eventually(i++).handle_exception_type([&] (std::bad_alloc&) {
            intercepted++;
        }).get());
    });
    BOOST_REQUIRE(intercepted);
}
#endif

SEASTAR_TEST_CASE(test_queue_pop_after_abort) {
    return async([] {
        queue<int> q(1);
        bool exception = false;
        bool timer = false;
        future<> done = make_ready_future();
        q.abort(std::make_exception_ptr(std::runtime_error("boom")));
        done = sleep(1ms).then([&] {
            timer = true;
            q.abort(std::make_exception_ptr(std::runtime_error("boom")));
        });
        try {
            q.pop_eventually().get();
        } catch(...) {
            exception = !timer;
        }
        BOOST_REQUIRE(exception);
        done.get();
    });
}

SEASTAR_TEST_CASE(test_queue_push_abort) {
    return async([] {
        queue<int> q(1);
        bool exception = false;
        bool timer = false;
        future<> done = make_ready_future();
        q.abort(std::make_exception_ptr(std::runtime_error("boom")));
        done = sleep(1ms).then([&] {
            timer = true;
            q.abort(std::make_exception_ptr(std::runtime_error("boom")));
        });
        try {
            q.push_eventually(1).get();
        } catch(...) {
            exception = !timer;
        }
        BOOST_REQUIRE(exception);
        done.get();
    });
}
