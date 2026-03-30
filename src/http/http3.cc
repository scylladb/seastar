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
#ifdef SEASTAR_HAVE_NGHTTP3

#include <seastar/http/http3.hh>
#include <seastar/http/request.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/routes.hh>
#include <seastar/net/quic.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/util/log.hh>
#include <seastar/util/string_utils.hh>

#include <nghttp3/nghttp3.h>
#include <fmt/format.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace seastar::http::experimental {

static seastar::logger h3log("http3");

namespace {

// Bring the QUIC namespace into scope for this translation unit.
namespace quic = seastar::experimental::quic;

// Per-stream request/response state, kept alive until the stream is fully handled.
struct stream_ctx {
    std::unique_ptr<http::request> req = std::make_unique<http::request>();
    std::string body;           // accumulates DATA frame payload
    bool request_ready = false; // set by end_stream callback
    sstring resp_body;          // populated before submit_response; read by cb_read_data
};

// Manages a single HTTP/3 connection: one nghttp3_conn over one quic::connection.
// Dispatches incoming requests to httpd::routes and sends HTTP/3 responses.
//
// IMPORTANT: this object must not be moved after run() is called, because
// nghttp3 callbacks hold a raw `this` pointer (conn_user_data).
class http3_connection {
    quic::connection _conn;
    std::unique_ptr<nghttp3_conn, decltype(&nghttp3_conn_del)> _h3conn{nullptr, nghttp3_conn_del};
    httpd::routes& _routes;
    seastar::gate _gate;
    std::unordered_map<int64_t, stream_ctx> _ctxs;
    // Reused across flush_h3() calls to avoid a heap allocation per response.
    std::vector<uint8_t> _flush_buf;

public:
    explicit http3_connection(quic::connection conn, httpd::routes& routes)
        : _conn(std::move(conn)), _routes(routes) {}

    // Runs the connection lifecycle: setup, accept-streams loop, cleanup.
    // Returns when the connection closes.
    future<> run();

private:
    // nghttp3 callbacks (all receive conn_user_data = `this`)
    static int cb_recv_header(nghttp3_conn*, int64_t sid, int32_t token,
                              nghttp3_rcbuf* name, nghttp3_rcbuf* value,
                              uint8_t flags, void* ud, void*);
    static int cb_end_headers(nghttp3_conn*, int64_t, int, void*, void*);
    static int cb_recv_data(nghttp3_conn* h3, int64_t sid,
                            const uint8_t* data, size_t len,
                            void* ud, void*);
    static int cb_end_stream(nghttp3_conn*, int64_t sid, void* ud, void*);
    static int cb_recv_settings(nghttp3_conn*, const nghttp3_settings*, void*);
    // Data reader for response bodies.  Looks up the body via conn_user_data
    // + stream_id rather than stream_user_data (which is not set server-side).
    static nghttp3_ssize cb_read_data(nghttp3_conn*, int64_t sid,
                                      nghttp3_vec* vec, size_t veccnt,
                                      uint32_t* pflags, void* ud, void*);

    future<> handle_stream(quic::stream s);
    future<> send_response(int64_t sid, std::unique_ptr<http::reply> rep);
    // Drains nghttp3's pending output and sends it over the QUIC connection.
    // Re-entrant calls are safe: add_write_offset is called before the
    // co_await, so an interleaved caller sees only new frames.
    future<> flush_h3();
};

// Callbacks

int http3_connection::cb_recv_header(nghttp3_conn*, int64_t sid, int32_t token,
                                     nghttp3_rcbuf* name, nghttp3_rcbuf* value,
                                     uint8_t, void* ud, void*) {
    auto& self = *static_cast<http3_connection*>(ud);
    auto it = self._ctxs.find(sid);
    if (it == self._ctxs.end()) return 0;
    auto& req = *it->second.req;

    auto nv = nghttp3_rcbuf_get_buf(name);
    auto vv = nghttp3_rcbuf_get_buf(value);
    std::string_view n(reinterpret_cast<const char*>(nv.base), nv.len);
    std::string_view v(reinterpret_cast<const char*>(vv.base), vv.len);

    // HTTP/3 pseudo-headers: use the well-known QPACK token values to avoid
    // string comparison in the hot path.
    switch (token) {
    case NGHTTP3_QPACK_TOKEN__METHOD:
        req._method = sstring(v);
        break;
    case NGHTTP3_QPACK_TOKEN__PATH:
        req._url = sstring(v);
        break;
    case NGHTTP3_QPACK_TOKEN__AUTHORITY:
        // Map :authority to the Host header so existing handlers see a
        // familiar field.
        req._headers["Host"] = sstring(v);
        break;
    case NGHTTP3_QPACK_TOKEN__SCHEME:
        break; // always "https" for HTTP/3; not needed by handlers
    default:
        req._headers[sstring(n)] = sstring(v);
        break;
    }
    return 0;
}

int http3_connection::cb_end_headers(nghttp3_conn*, int64_t, int, void*, void*) {
    return 0;
}

int http3_connection::cb_recv_data(nghttp3_conn* h3, int64_t sid,
                                   const uint8_t* data, size_t len,
                                   void* ud, void*) {
    auto& self = *static_cast<http3_connection*>(ud);
    auto it = self._ctxs.find(sid);
    if (it != self._ctxs.end()) {
        it->second.body.append(reinterpret_cast<const char*>(data), len);
    }
    // Immediately ack DATA bytes so nghttp3 can release its internal buffer.
    nghttp3_conn_add_ack_offset(h3, sid, len);
    return 0;
}

int http3_connection::cb_end_stream(nghttp3_conn*, int64_t sid, void* ud, void*) {
    auto& self = *static_cast<http3_connection*>(ud);
    auto it = self._ctxs.find(sid);
    if (it != self._ctxs.end()) {
        it->second.request_ready = true;
    }
    return 0;
}

int http3_connection::cb_recv_settings(nghttp3_conn*, const nghttp3_settings*,
                                       void*) {
    return 0;
}

nghttp3_ssize http3_connection::cb_read_data(nghttp3_conn*, int64_t sid,
                                             nghttp3_vec* vec, size_t,
                                             uint32_t* pflags, void* ud, void*) {
    auto& self = *static_cast<http3_connection*>(ud);
    auto it = self._ctxs.find(sid);
    if (it == self._ctxs.end() || it->second.resp_body.empty()) {
        *pflags = NGHTTP3_DATA_FLAG_EOF;
        return 0;
    }
    auto& body = it->second.resp_body;
    vec[0].base = reinterpret_cast<uint8_t*>(body.data());
    vec[0].len = body.size();
    *pflags = NGHTTP3_DATA_FLAG_EOF;
    return 1;
}

// http3_connection methods

future<> http3_connection::run() {
    nghttp3_callbacks cbs{};
    cbs.recv_header   = cb_recv_header;
    cbs.end_headers   = cb_end_headers;
    cbs.recv_data     = cb_recv_data;
    cbs.end_stream    = cb_end_stream;
    cbs.recv_settings = cb_recv_settings;

    nghttp3_settings settings{};
    nghttp3_settings_default(&settings);

    {
        nghttp3_conn* raw = nullptr;
        if (auto rv = nghttp3_conn_server_new(&raw, &cbs, &settings, nullptr, this);
            rv != 0) {
            throw std::runtime_error(
                fmt::format("nghttp3_conn_server_new: {}", nghttp3_strerror(rv)));
        }
        _h3conn.reset(raw);
    }

    // Open the three server-initiated unidirectional streams required by HTTP/3:
    //   control stream (carries SETTINGS, GOAWAY, MAX_PUSH_ID)
    //   QPACK encoder stream (sends dynamic table entries)
    //   QPACK decoder stream (sends table acknowledgements)
    // Their IDs are assigned by ngtcp2 as consecutive server-uni IDs (3, 7, 11).
    auto ctrl = co_await _conn.open_uni_stream();
    auto qenc = co_await _conn.open_uni_stream();
    auto qdec = co_await _conn.open_uni_stream();

    if (auto rv = nghttp3_conn_bind_control_stream(_h3conn.get(), ctrl.id()); rv != 0) {
        throw std::runtime_error(
            fmt::format("nghttp3_conn_bind_control_stream: {}", nghttp3_strerror(rv)));
    }
    if (auto rv = nghttp3_conn_bind_qpack_streams(_h3conn.get(), qenc.id(), qdec.id());
        rv != 0) {
        throw std::runtime_error(
            fmt::format("nghttp3_conn_bind_qpack_streams: {}", nghttp3_strerror(rv)));
    }

    // Flush the initial SETTINGS + QPACK setup frames so the client can start
    // sending requests.
    co_await flush_h3();

    // Accept and dispatch client streams until the connection closes.
    try {
        for (;;) {
            auto s = co_await _conn.accept_stream();
            auto sid = s.id();
            // Non-coroutine lambda delegates to the named function: the lambda
            // executes synchronously (no co_await), so there is no closure
            // lifetime hazard from GCC's coroutine frame storage.
            (void)seastar::try_with_gate(_gate,
                [this, stream = std::move(s)]() mutable {
                    return handle_stream(std::move(stream));
                }).handle_exception_type([](const seastar::gate_closed_exception&) {})
                  .handle_exception([sid](std::exception_ptr ep) {
                    h3log.warn("stream {} error: {}", sid, ep);
                });
        }
    } catch (...) {
        // Connection closed by peer or idle timeout — normal shutdown path.
    }

    co_await _gate.close();
}

future<> http3_connection::handle_stream(quic::stream s) {
    auto sid = s.id();
    // Only client-initiated bidirectional streams (sid % 4 == 0) carry HTTP
    // requests.  Client-initiated unidirectional streams (sid % 4 == 2) are
    // control/QPACK streams; nghttp3 handles them internally.
    bool is_request = (sid % 4 == 0);
    if (is_request) {
        _ctxs.emplace(sid, stream_ctx{});
    }

    auto in = s.input();
    for (;;) {
        auto buf = co_await in.read();
        bool fin = buf.empty();
        auto nread = nghttp3_conn_read_stream(
            _h3conn.get(), sid,
            reinterpret_cast<const uint8_t*>(buf.get()), buf.size(),
            fin ? 1 : 0);
        if (nread < 0) {
            h3log.warn("read_stream(stream={}): {}", sid, nghttp3_strerror(nread));
            if (is_request) _ctxs.erase(sid);
            co_return;
        }
        if (fin) break;
    }

    if (!is_request) co_return;

    auto it = _ctxs.find(sid);
    if (it == _ctxs.end()) co_return;
    if (!it->second.request_ready) {
        _ctxs.erase(it);
        co_return;
    }

    auto& ctx = it->second;
    ctx.req->_version = "3.0";
    if (!ctx.body.empty()) {
        ctx.req->content_length = ctx.body.size();
        http::internal::deprecated_content(*ctx.req) =
            sstring(ctx.body.data(), ctx.body.size());
    }
    // Extract url before moving ctx.req: C++ does not guarantee argument
    // evaluation order, so std::move(ctx.req) may evaluate before ctx.req->_url
    // if both appear in the same call expression.
    sstring url = ctx.req->_url;

    auto rep = co_await _routes.handle(
        url, std::move(ctx.req), std::make_unique<http::reply>());

    // Move the response body into the stream context before calling
    // send_response so that cb_read_data can find it via _ctxs[sid].
    ctx.resp_body = std::move(rep->_content);

    co_await send_response(sid, std::move(rep));
    // Only erase after flush_h3() has sent all data (send_response awaits it).
    _ctxs.erase(sid);
}

future<> http3_connection::send_response(int64_t sid,
                                         std::unique_ptr<http::reply> rep) {
    auto status_str = std::to_string(static_cast<int>(rep->_status));

    // Build the nghttp3_nv header array.  :status MUST come first per RFC 9114.
    std::vector<nghttp3_nv> nva;
    nva.reserve(1 + rep->_headers.size());
    nva.push_back({
        reinterpret_cast<uint8_t*>(const_cast<char*>(":status")),
        reinterpret_cast<uint8_t*>(status_str.data()),
        7,
        status_str.size(),
        NGHTTP3_NV_FLAG_NO_COPY_NAME  // ":status" is a compile-time constant
    });

    for (auto& [k, v] : rep->_headers) {
        // HTTP/3 does not use Transfer-Encoding; body framing is via DATA frames.
        if (seastar::internal::case_insensitive_cmp()(k, "transfer-encoding")) {
            continue;
        }
        nva.push_back({
            reinterpret_cast<uint8_t*>(const_cast<char*>(k.c_str())),
            reinterpret_cast<uint8_t*>(const_cast<char*>(v.c_str())),
            k.size(),
            v.size(),
            0  // flag=0: nghttp3 copies name and value
        });
    }

    auto& ctx = _ctxs.at(sid);
    nghttp3_data_reader dr{cb_read_data};
    auto rv = nghttp3_conn_submit_response(
        _h3conn.get(), sid,
        nva.data(), nva.size(),
        ctx.resp_body.empty() ? nullptr : &dr);
    if (rv != 0) {
        h3log.warn("nghttp3_conn_submit_response(stream={}): {}",
                   sid, nghttp3_strerror(rv));
        co_return;
    }

    co_await flush_h3();
}

future<> http3_connection::flush_h3() {
    // This loop must not be re-entered concurrently: if two coroutines both
    // call writev_stream before either has called add_write_offset, nghttp3
    // returns the same pending data to both and the same frames are sent twice.
    //
    // Because seastar is single-threaded, "concurrently" here means interleaved
    // at a co_await point — specifically the co_await write_stream_data below.
    // Calling add_write_offset BEFORE the co_await removes the window: once we
    // release nghttp3's buffer, a concurrent flush_h3 caller sees only NEW data.
    // This makes the semaphore unnecessary.
    for (;;) {
        int64_t out_stream_id;
        int fin;
        nghttp3_vec vec[16];
        auto nwrite = nghttp3_conn_writev_stream(
            _h3conn.get(), &out_stream_id, &fin, vec, 16);
        if (nwrite < 0) {
            h3log.warn("nghttp3_conn_writev_stream: {}", nghttp3_strerror(nwrite));
            break;
        }
        if (nwrite == 0) break; // nothing pending right now

        // Copy into a contiguous buffer; the vec[] pointers into nghttp3's
        // memory become stale once add_write_offset is called below.
        // _flush_buf is a member so its capacity persists across calls,
        // avoiding a heap allocation on every flush.
        _flush_buf.clear();
        for (nghttp3_ssize i = 0; i < nwrite; i++) {
            _flush_buf.insert(_flush_buf.end(), vec[i].base, vec[i].base + vec[i].len);
        }
        size_t total = _flush_buf.size();

        // Release nghttp3's send buffer BEFORE yielding.  A concurrent
        // flush_h3 call (interleaved at the co_await below) will then see only
        // new data from nghttp3, not the same batch we just copied.
        if (auto rv = nghttp3_conn_add_write_offset(
                _h3conn.get(), out_stream_id, total);
            rv != 0) {
            h3log.warn("nghttp3_conn_add_write_offset(stream={}): {}",
                       out_stream_id, nghttp3_strerror(rv));
        }

        // Hand the bytes to the QUIC layer (may suspend at flow-control).
        co_await _conn.write_stream_data(
            out_stream_id, _flush_buf.data(), total, fin != 0);
    }
}

} // anonymous namespace

// http3_server::impl

class http3_server::impl {
    quic::server _srv;
    httpd::routes& _routes;
    seastar::gate _gate;

public:
    impl(quic::server srv, httpd::routes& routes)
        : _srv(std::move(srv)), _routes(routes) {}

    void start();
    future<> stop();
    socket_address local_address() const noexcept;

private:
    // Named coroutine: `this` is stored directly in the heap-allocated
    // coroutine frame.  Must NOT be a lambda coroutine: GCC stores the
    // lambda closure pointer (a stack address) in the frame, leaving a
    // dangling reference once the calling scope returns.
    future<> accept_loop();

    // Run a single HTTP/3 connection.  h3c is a value parameter so GCC
    // copies it into the coroutine frame — remains valid across suspensions.
    static future<> run_connection(lw_shared_ptr<http3_connection> h3c);
};

void http3_server::impl::start() {
    // Non-coroutine lambda: just forwards to accept_loop().  The lambda
    // closure is destroyed immediately after the call, which is fine because
    // accept_loop() stores `this` in its own coroutine frame.
    (void)try_with_gate(_gate, [this]() { return accept_loop(); })
        .handle_exception_type([](const seastar::gate_closed_exception&) {});
}

future<> http3_server::impl::accept_loop() {
    try {
        for (;;) {
            auto conn = co_await _srv.accept();
            auto h3c = seastar::make_lw_shared<http3_connection>(
                std::move(conn), _routes);
            (void)try_with_gate(_gate, [h3c]() { return run_connection(h3c); })
                .handle_exception([](std::exception_ptr ep) {
                    h3log.warn("http3 connection error: {}", ep);
                });
        }
    } catch (...) {
        // Server closed or gate closed — normal shutdown path.
    }
}

future<> http3_server::impl::run_connection(lw_shared_ptr<http3_connection> h3c) {
    co_await h3c->run();
}

future<> http3_server::impl::stop() {
    // Close the QUIC server: aborts the accept queue (exits our accept loop)
    // and sends connection-close to all active connections.
    co_await _srv.close();
    // Wait for all accept-loop and per-connection gate tasks to complete.
    co_await _gate.close();
}

socket_address http3_server::impl::local_address() const noexcept {
    return _srv.local_address();
}

// http3_server

http3_server::http3_server(http3_server&&) noexcept = default;
http3_server& http3_server::operator=(http3_server&&) noexcept = default;
http3_server::~http3_server() = default;
http3_server::http3_server(std::unique_ptr<impl> p) : _impl(std::move(p)) {}

future<http3_server> http3_server::listen(
    socket_address addr,
    shared_ptr<seastar::experimental::quic::credentials> creds,
    httpd::routes& routes,
    seastar::experimental::quic::connection_config config) {

    auto srv = co_await quic::server::listen(addr, std::move(creds), config);
    auto p = std::make_unique<http3_server::impl>(std::move(srv), routes);
    p->start();
    co_return http3_server(std::move(p));
}

future<> http3_server::stop() {
    return _impl->stop();
}

socket_address http3_server::local_address() const noexcept {
    return _impl->local_address();
}

} // namespace seastar::http::experimental

#endif // SEASTAR_HAVE_NGHTTP3
#endif // SEASTAR_HAVE_QUIC
