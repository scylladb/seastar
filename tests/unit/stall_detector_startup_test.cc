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
 * Copyright (C) 2026 ScyllaDB Ltd.
 */

// Checks that the stall detector reports a stall in the very first batch of
// tasks that the reactor runs. With one shard, the application's main
// function runs in that batch, so this test must run with -c1 (see
// CMakeLists.txt).

#include <seastar/core/app-template.hh>
#include <seastar/core/internal/stall_detector.hh>
#include <seastar/core/reactor.hh>
#include <seastar/util/log.hh>
#include <atomic>
#include <chrono>

using namespace seastar;
using namespace std::chrono_literals;

static logger testlog("testlog");

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        static std::atomic<unsigned> reports{0};
        reactor::test::set_stall_detector_report_function([] { ++reports; });
        // Stall for much longer than the threshold of 25 ms (see
        // CMakeLists.txt).
        auto end = internal::cpu_stall_detector::clock_type::now() + 500ms;
        while (internal::cpu_stall_detector::clock_type::now() < end) {
        }
        if (reports == 0) {
            testlog.error("the stall detector did not report a stall in the first batch of tasks");
            return make_ready_future<int>(1);
        }
        testlog.info("the stall detector reported {} times", reports.load());
        return make_ready_future<int>(0);
    });
}
