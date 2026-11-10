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
 * Copyright (C) 2026 ScyllaDB
 */

// Regression test for https://github.com/scylladb/seastar/issues/1784.
//
// The linux-aio backend polls network fds with IOCB_CMD_POLL iocbs on an aio
// context created with io_setup(max-networking-io-control-blocks). The kernel
// reserves one completion-ring slot per in-flight iocb at io_submit() time and
// frees it only when userspace reaps the completion. A poll on an idle fd
// never completes, so idle connections hold their slots indefinitely and the
// number of in-flight polls is bounded by the number of open fds, not by the
// configured budget. Once the ring is full io_submit() returns EAGAIN for as
// long as nothing is reaped; flush() used to spin on that for one second and
// abort.
//
// This test runs with a tiny budget (see RUN_ARGS in CMakeLists.txt), opens
// idle loopback connections until a connect() stalls because the ring is
// full, and then checks that the reactor is still alive and that closing
// idle connections frees enough slots for a new connection to succeed.

#include <seastar/core/coroutine.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/timed_out_error.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/net/api.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/log.hh>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <sys/resource.h>

using namespace seastar;
using namespace std::chrono_literals;

namespace {

logger tlog("aio_poll_ring_test");

// An idle connection with a read parked on it. The read never completes until
// the socket is shut down, so it keeps one POLLIN poll in flight in the ring.
struct idle_conn {
    connected_socket sock;
    input_stream<char> in;
    future<> read_done;

    explicit idle_conn(connected_socket s)
        : sock(std::move(s))
        , in(sock.input())
        , read_done(in.read().discard_result())
    {}

    // Wake the parked read with EOF and wait for it before the stream goes away.
    future<> close() {
        sock.shutdown_input();
        sock.shutdown_output();
        co_await std::move(read_done);
        co_await in.close();
    }
};

using conn_list = std::vector<std::unique_ptr<idle_conn>>;

future<> close_all(conn_list& conns) {
    for (auto& c : conns) {
        co_await c->close();
    }
    conns.clear();
}

future<> accept_loop(server_socket& listener, conn_list& servers, const bool& aborting) {
    while (true) {
        try {
            auto ar = co_await listener.accept();
            servers.push_back(std::make_unique<idle_conn>(std::move(ar.connection)));
        } catch (...) {
            if (aborting) {
                co_return;
            }
            // e.g. EMFILE: surface it instead of leaving connections unaccepted.
            throw;
        }
    }
}

// The test needs roughly as many fds as the completion ring has slots, which
// may exceed the default soft limit on machines with many CPUs.
void raise_fd_limit(rlim_t wanted) {
    rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < wanted) {
        rl.rlim_cur = std::min(wanted, rl.rlim_max);
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            tlog.warn("could not raise RLIMIT_NOFILE to {}: {}", rl.rlim_cur, strerror(errno));
        }
    }
}

} // anonymous namespace

SEASTAR_THREAD_TEST_CASE(test_poll_ring_overflow_does_not_abort) {
    if (engine().get_backend_name() != "linux-aio") {
        tlog.info("skipping: requires the linux-aio reactor backend");
        return;
    }

    // Each loopback connection parks two polls (client and server side). The
    // ring holds at least max(budget, 4 * num_possible_cpus) * 2 + 2 entries,
    // rounded up to a page, so this cap covers machines with up to ~1000 CPUs.
    constexpr unsigned max_conns = 4000;
    constexpr auto connect_timeout = 5s;
    raise_fd_limit(2 * max_conns + 64);

    // Keep every accepted connection on this shard. Connections handed to a
    // shard with no listener are closed, which would complete the client's
    // parked poll and free its ring slot.
    listen_options lo;
    lo.reuse_address = true;
    lo.set_fixed_cpu(this_shard_id());
    auto listener = listen(make_ipv4_address({0}), lo);
    auto addr = listener.local_address();

    // Parked reads must be woken and waited for before their streams are
    // destroyed, including when the test fails midway, so clean up in
    // deferred actions. They run in reverse order: clients, accept loop,
    // then servers. None of them is expected to fail; if one does, the
    // noexcept lambda terminates the test.
    conn_list servers;
    auto close_servers = defer([&] () noexcept {
        close_all(servers).get();
    });
    bool aborting = false;
    auto accept_done = accept_loop(listener, servers, aborting);
    auto stop_accepting = defer([&] () noexcept {
        aborting = true;
        listener.abort_accept();
        accept_done.get();
    });
    conn_list clients;
    auto close_clients = defer([&] () noexcept {
        close_all(clients).get();
    });

    bool stalled = false;
    for (unsigned i = 0; i < max_conns; ++i) {
        try {
            auto s = with_timeout(lowres_clock::now() + connect_timeout, connect(addr)).get();
            clients.push_back(std::make_unique<idle_conn>(std::move(s)));
        } catch (const timed_out_error&) {
            // The ring is full: the POLLOUT poll for this connect() could not
            // be submitted. Before the fix the reactor aborted here instead.
            tlog.info("connect() stalled after {} idle connections", clients.size());
            stalled = true;
            break;
        }
    }
    BOOST_REQUIRE(stalled);

    // Shutting down the client side completes its parked poll and makes the
    // server side see EOF, which completes that poll too. Reaping those frees
    // ring slots, so the queued iocbs get submitted and a new connect() works.
    auto to_close = clients.size() / 2;
    for (size_t i = 0; i < to_close; ++i) {
        clients[i]->close().get();
    }
    clients.erase(clients.begin(), clients.begin() + to_close);

    auto s = with_timeout(lowres_clock::now() + connect_timeout, connect(addr)).get();
    clients.push_back(std::make_unique<idle_conn>(std::move(s)));
}
