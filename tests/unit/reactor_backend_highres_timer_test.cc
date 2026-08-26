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
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/internal/poll.hh>
#include <seastar/testing/test_case.hh>

#include <chrono>

using namespace seastar;
using namespace std::chrono_literals;

// Like DPDK, this poller prevents the reactor from entering interrupt mode.
// The high-resolution timer must therefore be serviced while polling.
SEASTAR_TEST_CASE(highres_timer_with_active_poller_test) {
    auto active_poller = reactor::poller::simple([] {
        return false;
    });

    co_await sleep(100ms);
}
