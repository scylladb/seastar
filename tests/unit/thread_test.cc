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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#include <seastar/core/thread.hh>
#include <seastar/core/do_with.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/sleep.hh>

using namespace seastar;
using namespace std::chrono_literals;

SEASTAR_TEST_CASE(test_thread_1) {
    return do_with(sstring(), [] (sstring& x) {
        auto t1 = new thread([&x] {
            x = "abc";
        });
        return t1->join().then([&x, t1] {
            BOOST_REQUIRE_EQUAL(x, "abc");
            delete t1;
        });
    });
}

SEASTAR_TEST_CASE(test_thread_2) {
    struct tmp {
        std::vector<thread> threads;
        semaphore sem1{0};
        semaphore sem2{0};
        int counter = 0;
        void thread_fn() {
            sem1.wait(1).get();
            ++counter;
            sem2.signal(1);
        }
    };
    return do_with(tmp(), [] (tmp& x) {
        auto n = 10;
        for (int i = 0; i < n; ++i) {
            x.threads.emplace_back(std::bind(&tmp::thread_fn, &x));
        }
        BOOST_REQUIRE_EQUAL(x.counter, 0);
        x.sem1.signal(n);
        return x.sem2.wait(n).then([&x, n] {
            BOOST_REQUIRE_EQUAL(x.counter, n);
            return parallel_for_each(x.threads.begin(), x.threads.end(), std::mem_fn(&thread::join));
        });
    });
}

SEASTAR_TEST_CASE(test_thread_async) {
    sstring x = "x";
    sstring y = "y";
    auto concat = [] (sstring x, sstring y) {
        sleep(10ms).get();
        return x + y;
    };
    return async(concat, x, y).then([] (sstring xy) {
        BOOST_REQUIRE_EQUAL(xy, "xy");
    });
}

SEASTAR_TEST_CASE(test_thread_async_immed) {
    return async([] { return 3; }).then([] (int three) {
        BOOST_REQUIRE_EQUAL(three, 3);
    });
}

SEASTAR_TEST_CASE(test_thread_async_nested) {
    return async([] {
        return async([] {
            return 3;
        }).get0();
    }).then([] (int three) {
        BOOST_REQUIRE_EQUAL(three, 3);
    });
}

void compute(float& result, bool& done, uint64_t& ctr) {
    while (!done) {
        for (int n = 0; n < 10000; ++n) {
            result += 1 / (result + 1);
            ++ctr;
        }
        thread::yield();
    }
}

SEASTAR_TEST_CASE(test_thread_sched_group) {
    return async([] {
        float metered_ratio = 0.2;
        thread_scheduling_group sched_group(1ms, metered_ratio);
        float metered_result = 0;
        float unmetered_result = 0;
        bool done = false;
        uint64_t u_ctr = 0, m_ctr = 0;
        thread unmetered([&] { compute(unmetered_result, done, u_ctr); });
        thread_attributes metered_thread_attributes;
        metered_thread_attributes.scheduling_group = &sched_group;
        thread metered(metered_thread_attributes, [&] { compute(metered_result, done, m_ctr); });
        sleep(500ms).get();
        done = true;
        when_all(metered.join(), unmetered.join()).discard_result().get();
        auto ratio = float(m_ctr) / (u_ctr + m_ctr);
#ifndef SEASTAR_DEBUG
        BOOST_REQUIRE(ratio > metered_ratio - 0.05);
        BOOST_REQUIRE(ratio < metered_ratio + 0.05);
#else
        // debug mode is too slow to test this accurately
        (void)ratio;
#endif
    });
}

#if defined(SEASTAR_ASAN_ENABLED) && defined(SEASTAR_HAVE_ASAN_FIBER_SUPPORT)
volatile int force_write;
volatile void* shut_up_gcc;

[[gnu::noinline]]
void throw_exception() {
    volatile char buf[1024];
    shut_up_gcc = &buf;
    for (int i = 0; i < 1024; i++) {
        buf[i] = force_write;
    }
    throw 1;
}

[[gnu::noinline]]
void use_stack() {
    volatile char buf[2 * 1024];
    shut_up_gcc = &buf;
    for (int i = 0; i < 2 * 1024; i++) {
        buf[i] = force_write;
    }
}

SEASTAR_TEST_CASE(test_asan_false_positive) {
    return async([] {
        try {
            throw_exception();
        } catch (...) {
            use_stack();
        }
    });
}
#endif
