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

#if defined(SEASTAR_HAVE_QUIC) && defined(SEASTAR_HAVE_NGHTTP3)

#include <seastar/http/http3.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/routes.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/quic.hh>
#include <seastar/core/gate.hh>
#include <seastar/testing/test_case.hh>

#include <nghttp3/nghttp3.h>
#include "quic_creds.hh"

#include <memory>
#include <string>
#include <vector>

using namespace seastar;
using namespace seastar::experimental::quic;

struct h3_response {
    int status = 0;
    sstring body;
};

// Minimal HTTP/3 client: wraps a quic::connection and an nghttp3_conn.
// Supports one in-flight GET request at a time.
class h3_client {
    connection _conn;
    nghttp3_conn* _h3 = nullptr;
    h3_response _resp;
    bool _resp_done = false;
    std::vector<uint8_t> _flush_buf;

public:
    explicit h3_client(connection conn) : _conn(std::move(conn)) {}
    ~h3_client() { nghttp3_conn_del(_h3); }

    future<> setup();
    future<h3_response> get(const sstring& path);
    future<> close() { return _conn.close(); }

    // Accept the next server-initiated stream and feed its data through
    // nghttp3 until the stream closes.  Run three of these in background
    // after setup() to process the server's control + QPACK streams.
    future<> accept_and_pump();

private:
    static int cb_recv_header(nghttp3_conn*, int64_t, int32_t token,
                              nghttp3_rcbuf*, nghttp3_rcbuf* value,
                              uint8_t, void* ud, void*);
    static int cb_recv_data(nghttp3_conn* h3, int64_t sid,
                            const uint8_t* data, size_t len, void* ud, void*);
    static int cb_end_stream(nghttp3_conn*, int64_t, void* ud, void*);
    static int cb_end_headers(nghttp3_conn*, int64_t, int, void*, void*);
    static int cb_recv_settings(nghttp3_conn*, const nghttp3_settings*, void*);

    future<> flush();
};

int h3_client::cb_recv_header(nghttp3_conn*, int64_t, int32_t token,
                              nghttp3_rcbuf*, nghttp3_rcbuf* value,
                              uint8_t, void* ud, void*) {
    if (token == NGHTTP3_QPACK_TOKEN__STATUS) {
        auto v = nghttp3_rcbuf_get_buf(value);
        std::string s(reinterpret_cast<const char*>(v.base), v.len);
        static_cast<h3_client*>(ud)->_resp.status = std::stoi(s);
    }
    return 0;
}

int h3_client::cb_recv_data(nghttp3_conn* h3, int64_t sid,
                            const uint8_t* data, size_t len, void* ud, void*) {
    auto& self = *static_cast<h3_client*>(ud);
    self._resp.body += sstring(reinterpret_cast<const char*>(data), len);
    nghttp3_conn_add_ack_offset(h3, sid, len);
    return 0;
}

int h3_client::cb_end_stream(nghttp3_conn*, int64_t, void* ud, void*) {
    static_cast<h3_client*>(ud)->_resp_done = true;
    return 0;
}

int h3_client::cb_end_headers(nghttp3_conn*, int64_t, int, void*, void*) {
    return 0;
}

int h3_client::cb_recv_settings(nghttp3_conn*, const nghttp3_settings*, void*) {
    return 0;
}

future<> h3_client::setup() {
    nghttp3_callbacks cbs{};
    cbs.recv_header   = cb_recv_header;
    cbs.end_headers   = cb_end_headers;
    cbs.recv_data     = cb_recv_data;
    cbs.end_stream    = cb_end_stream;
    cbs.recv_settings = cb_recv_settings;

    nghttp3_settings settings{};
    nghttp3_settings_default(&settings);

    if (auto rv = nghttp3_conn_client_new(&_h3, &cbs, &settings, nullptr, this);
        rv != 0) {
        throw std::runtime_error("nghttp3_conn_client_new failed");
    }

    // Client must open 3 uni streams: control, QPACK encoder, QPACK decoder.
    auto ctrl = co_await _conn.open_uni_stream();
    auto qenc = co_await _conn.open_uni_stream();
    auto qdec = co_await _conn.open_uni_stream();

    nghttp3_conn_bind_control_stream(_h3, ctrl.id());
    nghttp3_conn_bind_qpack_streams(_h3, qenc.id(), qdec.id());

    // Flush SETTINGS frame to the server.
    co_await flush();
}

future<> h3_client::accept_and_pump() {
    auto s = co_await _conn.accept_stream();
    auto sid = s.id();
    auto in = s.input();
    for (;;) {
        auto buf = co_await in.read();
        bool fin = buf.empty();
        nghttp3_conn_read_stream(
            _h3, sid,
            reinterpret_cast<const uint8_t*>(buf.get()), buf.size(),
            fin ? 1 : 0);
        if (fin) break;
    }
}

future<h3_response> h3_client::get(const sstring& path) {
    _resp = {};
    _resp_done = false;

    // Open the request stream (client bidi, ID 0 for the first request).
    auto req_stream = co_await _conn.open_stream();
    auto req_id = req_stream.id();

    auto method    = std::string("GET");
    auto path_s    = std::string(path);
    auto scheme    = std::string("https");
    auto authority = std::string("localhost");

    nghttp3_nv nva[] = {
        {reinterpret_cast<uint8_t*>(const_cast<char*>(":method")),
         reinterpret_cast<uint8_t*>(method.data()), 7, method.size(), 0},
        {reinterpret_cast<uint8_t*>(const_cast<char*>(":path")),
         reinterpret_cast<uint8_t*>(path_s.data()), 5, path_s.size(), 0},
        {reinterpret_cast<uint8_t*>(const_cast<char*>(":scheme")),
         reinterpret_cast<uint8_t*>(scheme.data()), 7, scheme.size(), 0},
        {reinterpret_cast<uint8_t*>(const_cast<char*>(":authority")),
         reinterpret_cast<uint8_t*>(authority.data()), 10, authority.size(), 0},
    };

    // nullptr data reader = no request body; nghttp3 sends FIN with HEADERS.
    if (auto rv = nghttp3_conn_submit_request(_h3, req_id, nva, 4, nullptr, nullptr);
        rv != 0) {
        throw std::runtime_error("nghttp3_conn_submit_request failed");
    }

    co_await flush();

    // Read the server's response on the request stream.
    auto response_in = req_stream.input();
    for (;;) {
        auto buf = co_await response_in.read();
        bool fin = buf.empty();
        auto nread = nghttp3_conn_read_stream(
            _h3, req_id,
            reinterpret_cast<const uint8_t*>(buf.get()), buf.size(),
            fin ? 1 : 0);
        if (nread < 0) {
            throw std::runtime_error("nghttp3_conn_read_stream failed on response");
        }
        if (fin || _resp_done) break;
    }

    co_return std::move(_resp);
}

future<> h3_client::flush() {
    for (;;) {
        int64_t out_sid;
        int fin;
        nghttp3_vec vec[16];
        auto nwrite = nghttp3_conn_writev_stream(_h3, &out_sid, &fin, vec, 16);
        if (nwrite <= 0) break;

        size_t total = 0;
        for (nghttp3_ssize i = 0; i < nwrite; i++) total += vec[i].len;
        _flush_buf.resize(total);
        size_t off = 0;
        for (nghttp3_ssize i = 0; i < nwrite; i++) {
            std::memcpy(_flush_buf.data() + off, vec[i].base, vec[i].len);
            off += vec[i].len;
        }
        nghttp3_conn_add_write_offset(_h3, out_sid, total);
        co_await _conn.write_stream_data(out_sid, _flush_buf.data(), total, fin != 0);
    }
}

// ============================================================
// Test: basic HTTP/3 GET request and response
// ============================================================

SEASTAR_TEST_CASE(test_http3_get) {
    httpd::routes routes;
    routes.add(httpd::operation_type::GET, httpd::url("/hello"),
        new httpd::function_handler([](httpd::const_req) {
            return sstring("Hello, HTTP/3!");
        }, "txt"));

    socket_address addr(net::inet_address("127.0.0.1"), 0);
    auto srv = co_await http::experimental::http3_server::listen(
        addr, make_server_creds("h3"), routes);
    auto bound = srv.local_address();

    auto cli = std::make_unique<h3_client>(
        co_await connect(bound, make_client_creds("h3")));
    co_await cli->setup();

    // The server opens 3 uni streams for HTTP/3 setup (control + QPACK
    // encoder/decoder) and sends its SETTINGS frame.  Pump them through
    // nghttp3 in the background so SETTINGS and QPACK state are processed.
    // The pumps block on read() until the stream or connection closes, so
    // we use a gate and let cli->close() unblock them via connection teardown.
    seastar::gate pump_gate;
    for (int i = 0; i < 3; ++i) {
        // Discard exceptions: the pumps throw when the connection closes.
        (void)seastar::with_gate(pump_gate, [&cli]() {
            return cli->accept_and_pump();
        }).handle_exception([](std::exception_ptr) {});
    }

    auto resp = co_await cli->get("/hello");

    BOOST_CHECK_EQUAL(resp.status, 200);
    BOOST_CHECK(resp.body.find("Hello, HTTP/3!") != sstring::npos);

    co_await cli->close();
    co_await pump_gate.close();
    co_await srv.stop();
}

#else

#include <seastar/testing/test_case.hh>

SEASTAR_TEST_CASE(test_http3_not_built) {
    co_return;
}

#endif
