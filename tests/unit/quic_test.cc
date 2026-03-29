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
 * Copyright (C) 2026 Kefu Chai (tchaikov@gmail.com)
 */

#ifdef SEASTAR_HAVE_QUIC

#include <seastar/core/future.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/when_all.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/quic.hh>
#include <seastar/testing/test_case.hh>

#include "quic_creds.hh"

#include <fmt/format.h>
#include <string>

using namespace seastar;
using namespace seastar::experimental::quic;

/// Read an input_stream until EOF and return all data as an sstring.
static future<sstring> drain(input_stream<char>& in) {
    sstring result;
    for (;;) {
        auto buf = co_await in.read();
        if (buf.empty()) {
            break;
        }
        result += sstring(buf.get(), buf.size());
    }
    co_return result;
}

/// Listen on a random loopback port and return the bound server + address.
static future<std::pair<server, socket_address>> make_server() {
    // Port 0 lets the kernel pick a free port.
    socket_address addr(net::inet_address("127.0.0.1"), 0);
    auto srv = co_await server::listen(addr, make_server_creds());
    auto bound = srv.local_address();
    co_return std::make_pair(std::move(srv), bound);
}

// ============================================================
// Test: basic loopback connection handshake
// ============================================================

SEASTAR_TEST_CASE(test_quic_connect) {
    auto [srv, bound] = co_await make_server();

    // Accept and connect concurrently — handshake on both sides.
    auto [server_conn, client_conn] = co_await when_all_succeed(
            srv.accept(),
            connect(bound, make_client_creds()));

    co_await when_all_succeed(server_conn.close(), client_conn.close());
    co_await srv.close();
}

// ============================================================
// Test: client sends, server reads (echo)
// ============================================================

SEASTAR_TEST_CASE(test_quic_client_to_server) {
    auto [srv, bound] = co_await make_server();

    auto [server_conn, client_conn] = co_await when_all_succeed(
            srv.accept(),
            connect(bound, make_client_creds()));

    // Client opens a stream and sends a message.
    auto client_stream = co_await client_conn.open_stream();
    auto out = client_stream.output();
    co_await out.write("hello, QUIC");
    co_await out.flush();
    co_await out.close();

    // Server accepts the stream and reads it back.
    auto server_stream = co_await server_conn.accept_stream();
    auto in = server_stream.input();
    auto received = co_await drain(in);

    BOOST_CHECK_EQUAL(received, "hello, QUIC");

    co_await when_all_succeed(server_conn.close(), client_conn.close());
    co_await srv.close();
}

// ============================================================
// Test: server sends, client receives
// ============================================================

SEASTAR_TEST_CASE(test_quic_server_to_client) {
    auto [srv, bound] = co_await make_server();

    auto [server_conn, client_conn] = co_await when_all_succeed(
            srv.accept(),
            connect(bound, make_client_creds()));

    // Server opens a stream and sends data.
    auto server_stream = co_await server_conn.open_stream();
    auto out = server_stream.output();
    co_await out.write("from server");
    co_await out.flush();
    co_await out.close();

    // Client accepts the peer-initiated stream and reads.
    auto client_stream = co_await client_conn.accept_stream();
    auto in = client_stream.input();
    auto received = co_await drain(in);

    BOOST_CHECK_EQUAL(received, "from server");

    co_await when_all_succeed(server_conn.close(), client_conn.close());
    co_await srv.close();
}

// ============================================================
// Test: multiple concurrent streams on one connection
// ============================================================

// Free-function coroutines so that all parameters (conn reference, idx) are
// copied into the heap-allocated coroutine frame.  IIFE lambda coroutines
// store `this` as a raw pointer to the closure on the caller's stack; when
// the closure is a temporary the pointer becomes dangling after the first
// suspension point.
static future<> write_one_stream(connection& conn, int idx) {
    auto s = co_await conn.open_stream();
    auto out = s.output();
    co_await out.write(fmt::format("stream-{}", idx));
    co_await out.flush();
    co_await out.close();
}

static future<> read_one_stream(connection& conn) {
    auto s = co_await conn.accept_stream();
    auto in = s.input();
    auto received = co_await drain(in);
    BOOST_CHECK(received.starts_with("stream-"));
}

// Accept and verify num_streams streams sequentially.  seastar's queue<T>
// supports only one pending pop_eventually() at a time, so concurrent
// accept_stream() calls on the same connection are not allowed.
static future<> read_n_streams(connection& conn, int n) {
    for (int i = 0; i < n; ++i) {
        co_await read_one_stream(conn);
    }
}

SEASTAR_TEST_CASE(test_quic_multiple_streams) {
    static constexpr int num_streams = 5;

    auto [srv, bound] = co_await make_server();

    auto [server_conn, client_conn] = co_await when_all_succeed(
            srv.accept(),
            connect(bound, make_client_creds()));

    // Open num_streams streams from the client; send a unique message on each.
    // Writers run concurrently.
    std::vector<future<>> writers;
    writers.reserve(num_streams);
    for (int i = 0; i < num_streams; ++i) {
        writers.push_back(write_one_stream(client_conn, i));
    }

    // Accept and verify all streams on the server side.
    // Streams must be accepted sequentially: seastar's queue<T> supports only
    // one pending pop_eventually() at a time.
    auto reader = read_n_streams(server_conn, num_streams);

    co_await when_all_succeed(writers.begin(), writers.end());
    co_await std::move(reader);

    co_await when_all_succeed(server_conn.close(), client_conn.close());
    co_await srv.close();
}

#else // !SEASTAR_HAVE_QUIC

// Provide a stub so the test binary builds even without QUIC support.
#include <seastar/testing/test_case.hh>

SEASTAR_TEST_CASE(test_quic_not_built) {
    // QUIC support was not compiled in; nothing to test.
    co_return;
}

#endif // SEASTAR_HAVE_QUIC
