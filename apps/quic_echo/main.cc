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

/// Simple QUIC echo server for interoperability testing.
///
/// Usage:
///   quic_echo --cert <cert.crt> --key <key.pem> [--port <port>]
///
/// For each accepted stream it reads all incoming data, prints it to stdout,
/// and echoes it back to the peer.

#ifdef SEASTAR_HAVE_QUIC

#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/quic.hh>
#include <seastar/util/log.hh>

#include <fmt/format.h>
#include <cstdio>

using namespace seastar;
using namespace seastar::experimental::quic;

static seastar::logger qlog("quic_echo");

/// Handle one stream: read everything, echo it back.
static future<> handle_stream(stream s) {
    auto in  = s.input();
    auto out = s.output();
    size_t total = 0;

    for (;;) {
        auto buf = co_await in.read();
        if (buf.empty()) {
            break;
        }
        total += buf.size();
        co_await out.write(buf.get(), buf.size());
        co_await out.flush();
    }

    if (total > 0) {
        fmt::print("[stream] echoed {} bytes\n", total);
        std::fflush(stdout);
    }
    co_await out.close();
}

/// Accept all streams from one connection and dispatch them concurrently.
static future<> handle_connection(connection conn) {
    for (;;) {
        stream s = co_await conn.accept_stream();
        // Fire-and-forget each stream handler.
        (void)handle_stream(std::move(s)).handle_exception([](std::exception_ptr ep) {
            qlog.warn("stream handler error: {}", ep);
        });
    }
}

int main(int argc, char** argv) {
    app_template app;
    namespace po = boost::program_options;

    app.add_options()
        ("cert", po::value<std::string>()->required(),  "TLS certificate file (PEM)")
        ("key",  po::value<std::string>()->required(),  "TLS private key file (PEM)")
        ("port", po::value<uint16_t>()->default_value(4433), "UDP port to listen on");

    return app.run(argc, argv, [&app]() -> future<> {
        auto& config = app.configuration();
        auto cert_file = config["cert"].as<std::string>();
        auto key_file  = config["key"].as<std::string>();
        auto port      = config["port"].as<uint16_t>();

        auto creds = make_shared<credentials>();
        creds->set_x509_key_file(cert_file, key_file);
        // Use "h3" ALPN so standard ngtcp2 example clients (gtlsclient) can
        // connect.  Change to "quic" (or any custom token) when both ends are
        // under your control.
        creds->set_alpn("h3");

        socket_address addr(net::inet_address("0.0.0.0"), port);
        auto srv = co_await server::listen(addr, creds);
        fmt::print("QUIC echo server listening on port {}\n", port);
        fmt::print("Press Ctrl+C to stop.\n");
        std::fflush(stdout);

        for (;;) {
            auto conn = co_await srv.accept();
            fmt::print("[conn] accepted new connection\n");
            std::fflush(stdout);
            (void)handle_connection(std::move(conn)).handle_exception([](std::exception_ptr ep) {
                qlog.warn("connection handler error: {}", ep);
            });
        }
    });
}

#else  // !SEASTAR_HAVE_QUIC

#include <cstdio>

int main() {
    fprintf(stderr, "quic_echo: QUIC support was not compiled in.\n");
    return 1;
}

#endif
