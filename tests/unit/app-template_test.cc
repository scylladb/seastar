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
 * Copyright (C) 2023 ScyllaDB
 */

#define BOOST_TEST_MODULE app_template

#include <string>
#include <boost/test/unit_test.hpp>
#include <seastar/core/app-template.hh>
#include <seastar/core/sleep.hh>

using namespace seastar;
using namespace std::chrono_literals;

// #2148 - always run this.
BOOST_AUTO_TEST_CASE(app_standard_memory_allocator) {
    // by default, use conservative settings instead of maxing out the performance
    // for testing app_template and underlying reactor's handling of different
    // settings
    app_template::seastar_options opts;
    opts.smp_opts.thread_affinity.set_value(false);
    opts.smp_opts.mbind.set_value(false);
    opts.smp_opts.smp.set_value(1);
    opts.smp_opts.lock_memory.set_value(false);
    opts.smp_opts.memory_allocator = memory_allocator::standard;
    opts.log_opts.default_log_level.set_value(log_level::error);
    app_template app{std::move(opts)};
    // app.run() takes `char**` not `char* const *`, so appease it
    std::string prog_name{"prog"};
    char* args[] = {prog_name.data()};
    int expected_status = 42;
    int actual_status = app.run(
        std::size(args), std::data(args),
        [expected_status] {
            // #2148 - add a small sleep to ensure the reactor does
            // some of its background stuff, pollers etc for example.
            // We only need to ensure we get put on the waiting task queue
            // to provoke the problem, thus a short (probably even to long here)
            // sleep will do.
            return seastar::sleep(2s).then([expected_status] {
                return make_ready_future<int>(expected_status);
            });
        });
    BOOST_CHECK_EQUAL(actual_status, expected_status);
}

BOOST_AUTO_TEST_CASE(return_0_for_func_returning_void) {
    app_template app;
    std::string prog_name{"prog"};
    char* args[] = {prog_name.data()};
    int status = app.run(std::size(args), std::data(args),
                         [] { return make_ready_future(); });
    BOOST_CHECK_EQUAL(status, 0);
}

BOOST_AUTO_TEST_CASE(return_status_for_func_returning_int) {
    app_template app;
    std::string prog_name{"prog"};
    char* args[] = {prog_name.data()};
    int expected_status = 42;
    int actual_status = app.run(
        std::size(args), std::data(args),
         [expected_status] {
             return make_ready_future<int>(expected_status);
         });
    BOOST_CHECK_EQUAL(actual_status, expected_status);
}
