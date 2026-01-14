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
 * Copyright (C) 2026 Redpanda Data
 */

#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/when_all.hh>
#include <seastar/net/api.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <sys/socket.h>

using namespace seastar;

// Tests the issue described and fixed by
// https://github.com/scylladb/seastar/pull/1945 /
// 25e2487da55fb37d39ecd72e68c8e9d87f784c62 and to a degree also
// https://github.com/scylladb/seastar/pull/3204
//
// Sockets which errored out via EPOLLERR or EPOLLHUP would cause busy spinning
// in the reactor until eventually closed.
SEASTAR_THREAD_TEST_CASE(epoll_busy_spin_on_socket_error_test) {
    auto start_busy_time = engine().total_busy_time();

    listen_options lo;
    lo.reuse_address = true;
    server_socket ss = seastar::listen(ipv4_addr(0), lo);

    auto server = seastar::async([&] {
        accept_result acc = ss.accept().get();
        auto in = acc.connection.input();

        auto buf = in.read().get();

        // Now sleep, client will have sent RST in the meantime
        // Reactor will busy spin during this time.
        seastar::sleep(std::chrono::seconds(3)).get();

        in.close().get();
    });

    auto client = seastar::async([&] {
        connected_socket socket = connect(ss.local_address()).get();
        auto out = socket.output();

        out.write("hello world").get();
        out.flush().get();

        // 0 linger forces RST on close
        linger linger_opt{};
        linger_opt.l_onoff = 1;
        linger_opt.l_linger = 0;
        socket.set_sockopt(SOL_SOCKET, SO_LINGER, &linger_opt, sizeof(linger_opt));

        out.close().get();
    });

    when_all(std::move(server), std::move(client)).get();

    auto end_busy_time = engine().total_busy_time();
    auto busy_duration = end_busy_time - start_busy_time;
    auto busy_ms = std::chrono::duration_cast<std::chrono::milliseconds>(busy_duration).count();

    // Expectation is that we would busy spin for 3 seconds, we check that we
    // are below 1.5 seconds to avoid any kind of flakiness.
    BOOST_REQUIRE_LT(busy_ms, 1500);
}
