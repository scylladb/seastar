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
#include <seastar/core/queue.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sleep.hh>

using namespace seastar;
using namespace std::chrono_literals;

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
