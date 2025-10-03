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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

#include <seastar/core/timer.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/print.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <iostream>

using namespace seastar;
using namespace std::chrono_literals;

#define BUG() do { \
        std::cerr << "ERROR @ " << __FILE__ << ":" << __LINE__ << std::endl; \
        throw std::runtime_error("test failed"); \
    } while (0)

#define OK() do { \
        std::cerr << "OK @ " << __FILE__ << ":" << __LINE__ << std::endl; \
    } while (0)

template <typename Clock>
void test_timer_basic() {
    timer<Clock> t1;
    timer<Clock> t2;
    timer<Clock> t3;
    timer<Clock> t4;
    timer<Clock> t5;
    promise<> pr1;

        t1.set_callback([&] {
            OK();
            fmt::print(" 500ms timer expired\n");
            if (!t4.cancel()) {
                BUG();
            }
            if (!t5.cancel()) {
                BUG();
            }
            t5.arm(1100ms);
        });
        t2.set_callback([] { OK(); fmt::print(" 900ms timer expired\n"); });
        t3.set_callback([] { OK(); fmt::print("1000ms timer expired\n"); });
        t4.set_callback([] { OK(); fmt::print("  BAD cancelled timer expired\n"); });
        t5.set_callback([&pr1] { OK(); fmt::print("1600ms rearmed timer expired\n"); pr1.set_value(); });

        t1.arm(500ms);
        t2.arm(900ms);
        t3.arm(1000ms);
        t4.arm(700ms);
        t5.arm(800ms);

    pr1.get_future().get();
}

SEASTAR_THREAD_TEST_CASE(test_timer_basic_steady) {
    test_timer_basic<steady_clock_type>();
}

SEASTAR_THREAD_TEST_CASE(test_timer_basic_lowres) {
    test_timer_basic<lowres_clock>();
}

template <typename Clock>
void test_timer_cancelling() {
    promise<> pr2;

        timer<Clock> t1;
        t1.set_callback([] { BUG(); });
        t1.arm(100ms);
        t1.cancel();

        t1.arm(100ms);
        t1.cancel();

        t1.set_callback([&pr2] { OK(); pr2.set_value(); });
        t1.arm(100ms);

    pr2.get_future().get();
}

SEASTAR_THREAD_TEST_CASE(test_timer_cancelling_steady) {
    test_timer_cancelling<steady_clock_type>();
}

SEASTAR_THREAD_TEST_CASE(test_timer_cancelling_lowres) {
    test_timer_cancelling<lowres_clock>();
}

template <typename Clock>
void test_timer_with_scheduling_groups() {
            auto sg1 = create_scheduling_group("sg1", 100).get();
            auto sg2 = create_scheduling_group("sg2", 100).get();
            thread_attributes t1attr;
            t1attr.sched_group = sg1;
            auto expirations = 0;
            async(t1attr, [&] {
                auto make_callback_checking_sg = [&] (scheduling_group sg_to_check) {
                    return [sg_to_check, &expirations] {
                        ++expirations;
                        if (current_scheduling_group() != sg_to_check) {
                            BUG();
                        }
                    };
                };
                timer<Clock> t1(make_callback_checking_sg(sg1));
                t1.arm(10ms);
                timer<Clock> t2(sg2, make_callback_checking_sg(sg2));
                t2.arm(10ms);
                sleep(500ms).get();
                if (expirations != 2) {
                    BUG();
                }
                OK();
            }).get();
            destroy_scheduling_group(sg1).get();
            destroy_scheduling_group(sg2).get();
}

SEASTAR_THREAD_TEST_CASE(test_timer_with_scheduling_groups_steady) {
    test_timer_with_scheduling_groups<steady_clock_type>();
}

SEASTAR_THREAD_TEST_CASE(test_timer_with_scheduling_groups_lowres) {
    test_timer_with_scheduling_groups<lowres_clock>();
}
