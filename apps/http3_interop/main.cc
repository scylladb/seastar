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

/// HTTP/3 / hq-interop server compatible with the QUIC Interop Runner.
///
/// https://github.com/quic-interop/quic-interop-runner
///
/// Advertises both ALPN "hq-interop" and "h3".  The negotiated ALPN
/// determines which protocol handler runs per connection:
///
///   hq-interop — HTTP/0.9 over QUIC (most test cases):
///     Client sends:  GET /filename\r\n
///     Server responds with the raw bytes of $WWW/filename and closes.
///
///   h3 — HTTP/3 (RFC 9114):
///     Standard HTTP/3 request/response via seastar's http3_server.
///
/// Environment variables set by the interop runner:
///   TESTCASE  - test case name; exit 127 if not in the supported list.
///   WWW       - directory of files to serve (default: /www)
///   CERTS     - directory with cert.pem and priv.key (default: /certs)
///   SSLKEYLOGFILE - path to write TLS key log (not yet implemented)
///   QLOGDIR   - directory for qlog output (not yet implemented)
///
/// Local testing:
///   ./http3_interop --cert cert.pem --key key.pem --www /tmp/www --port 4433

#if defined(SEASTAR_HAVE_QUIC) && defined(SEASTAR_HAVE_NGHTTP3)

#include <seastar/http/http3.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/quic.hh>
#include <seastar/util/log.hh>
#include "../lib/stop_signal.hh"

#include <fmt/format.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <string_view>

using namespace seastar;
using namespace seastar::experimental::quic;

static seastar::logger ilog("http3_interop");

// Test cases this server supports.
static const std::array<std::string_view, 5> supported_testcases = {
    "handshake", "transfer", "longrtt", "multiplexing", "http3",
};

static constexpr std::string_view alpn_hq_interop = "hq-interop";
static constexpr std::string_view alpn_h3         = "h3";

static sstring env_or(const char* var, const char* def) {
    const char* v = std::getenv(var);
    return v ? sstring(v) : sstring(def);
}

// ── hq-interop (HTTP/0.9 over QUIC) ──────────────────────────────────────

/// Serve one hq-interop stream: parse "GET /path\r\n", stream the file back.
///
/// Named function (not a lambda) so GCC's coroutine frame stores `www_root`
/// by value, not as a pointer to a temporary closure.
static future<> handle_hq_stream(seastar::experimental::quic::stream s, sstring www_root) {
    auto in  = s.input();
    auto out = s.output();

    // Read the request line.  hq-interop sends "GET /path\r\n" or "GET /path\n".
    // Use std::string so append() is amortized O(1) rather than O(n) per buf.
    std::string line;
    for (;;) {
        auto buf = co_await in.read();
        if (buf.empty()) {
            co_await out.close();
            co_return;
        }
        line.append(buf.get(), buf.size());
        if (line.find('\n') != std::string::npos) break;
    }

    if (line.size() < 5 || line.compare(0, 4, "GET ") != 0) {
        co_await out.close();
        co_return;
    }

    size_t path_end = 4;
    while (path_end < line.size() && line[path_end] != '\r' && line[path_end] != '\n') {
        ++path_end;
    }
    // strip leading slash so www_root + "/" + rel_path forms a valid path
    size_t path_start = (line[4] == '/') ? 5 : 4;
    sstring rel_path(line.data() + path_start, path_end - path_start);

    sstring full_path = www_root + "/" + rel_path;
    ilog.debug("hq GET /{} → {}", rel_path, full_path);

    try {
        auto f = co_await open_file_dma(full_path, open_flags::ro);
        auto fstream = make_file_input_stream(std::move(f));
        for (;;) {
            auto buf = co_await fstream.read();
            if (buf.empty()) break;
            co_await out.write(buf.get(), buf.size());
        }
        co_await out.flush();
    } catch (const std::exception& e) {
        ilog.warn("hq: failed to serve {}: {}", full_path, e.what());
    }

    co_await out.close();
}

/// Accept all streams from one hq-interop connection.
static future<> run_hq_connection(connection conn, sstring www_root) {
    try {
        for (;;) {
            auto s = co_await conn.accept_stream();
            (void)handle_hq_stream(std::move(s), www_root)
                .handle_exception([](std::exception_ptr ep) {
                    ilog.warn("hq stream error: {}", ep);
                });
        }
    } catch (...) {
        // Connection closed — expected.
    }
}

// ── shared accept loop ────────────────────────────────────────────────────

/// Accept connections and dispatch based on negotiated ALPN.
///
/// Named free function so the coroutine frame stores its parameters by value
/// (not as pointers to a temporary closure).
static future<> run_accept_loop(server& srv, sstring www) {
    try {
        for (;;) {
            auto conn = co_await srv.accept();
            auto alpn = sstring(conn.negotiated_alpn());
            ilog.debug("accepted connection, ALPN={}", alpn);

            if (alpn == alpn_hq_interop) {
                (void)run_hq_connection(std::move(conn), www)
                    .handle_exception([](std::exception_ptr ep) {
                        ilog.warn("hq connection error: {}", ep);
                    });
            } else {
                ilog.warn("unknown ALPN '{}', closing connection", alpn);
                (void)conn.close().handle_exception([](std::exception_ptr) {});
            }
        }
    } catch (...) {
        // srv.close() was called — normal shutdown.
    }
}

// ── main ─────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // Check TESTCASE before Seastar initialises — exit 127 means "not supported".
    if (const char* tc = std::getenv("TESTCASE")) {
        if (!std::ranges::contains(supported_testcases, std::string_view(tc))) {
            fmt::print(stderr, "http3_interop: TESTCASE '{}' not supported\n", tc);
            return 127;
        }
    }

    app_template app;
    namespace po = boost::program_options;
    app.add_options()
        ("cert", po::value<std::string>(), "TLS certificate (default: $CERTS/cert.pem)")
        ("key",  po::value<std::string>(), "TLS private key  (default: $CERTS/priv.key)")
        ("www",  po::value<std::string>(), "Document root    (default: $WWW or /www)")
        ("port", po::value<uint16_t>()->default_value(443), "UDP port");

    return app.run(argc, argv, [&app]() -> future<> {
        auto& cfg = app.configuration();

        sstring certs_dir = env_or("CERTS", "/certs");
        sstring cert = cfg.count("cert")
            ? sstring(cfg["cert"].as<std::string>())
            : certs_dir + "/cert.pem";
        sstring key = cfg.count("key")
            ? sstring(cfg["key"].as<std::string>())
            : certs_dir + "/priv.key";
        sstring www = cfg.count("www")
            ? sstring(cfg["www"].as<std::string>())
            : env_or("WWW", "/www");
        uint16_t port = cfg["port"].as<uint16_t>();

        auto creds = make_shared<credentials>();
        creds->set_x509_key_file(cert, key);
        creds->set_alpn(std::string(alpn_hq_interop));
        creds->add_alpn(std::string(alpn_h3));

        socket_address addr(net::inet_address("0.0.0.0"), port);
        auto srv = co_await server::listen(addr, creds);
        fmt::print("HTTP/3 interop server on UDP port {}, www='{}'\n", port, www);
        std::fflush(stdout);

        // Track the accept loop so we can wait for it during shutdown.
        gate accept_gate;
        // Non-coroutine lambda calls the named coroutine — no closure lifetime
        // hazard because this lambda returns immediately with a future<>.
        (void)with_gate(accept_gate, [&srv, &www]() {
            return run_accept_loop(srv, www);
        }).handle_exception([](std::exception_ptr ep) {
            ilog.warn("accept loop error: {}", ep);
        });

        // Wait for SIGINT or SIGTERM.
        seastar_apps_lib::stop_signal stop_signal;
        co_await stop_signal.wait();
        co_await srv.close();          // unblocks srv.accept() in run_accept_loop
        co_await accept_gate.close();  // wait for the loop to drain
    });
}

#else  // !(SEASTAR_HAVE_QUIC && SEASTAR_HAVE_NGHTTP3)

#include <cstdio>

int main() {
    fprintf(stderr, "http3_interop: QUIC/HTTP3 support was not compiled in.\n");
    return 1;
}

#endif
