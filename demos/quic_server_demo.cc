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

#include <arpa/inet.h>

#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>
#include <seastar/quic/sharded_quic_server.hh>

#include "../apps/lib/stop_signal.hh"

using namespace seastar;
using namespace seastar::quic::experimental;
namespace bpo = boost::program_options;

static socket_address parse_ip_address(const std::string& ip, uint16_t port) {
    sockaddr_in6 sa6{};
    sa6.sin6_family = AF_INET6;
    sa6.sin6_port = htons(port);
    if (inet_pton(AF_INET6, ip.c_str(), &sa6.sin6_addr) == 1) {
        return socket_address(sa6);
    }

    sockaddr_in sa4{};
    sa4.sin_family = AF_INET;
    sa4.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &sa4.sin_addr) == 1) {
        return socket_address(sa4);
    }

    throw std::runtime_error("Invalid IP address: " + ip);
}

static future<> handle_stream(seastar::quic::experimental::stream quic_stream, bool verbose, uint64_t conn_id, uint64_t stream_no) {
    try {
        auto input = quic_stream.input();
        auto output = quic_stream.output();
        while (true) {
            auto chunk = co_await input.read();
            if (chunk.empty()) {
                break;
            }
            if (verbose) {
                std::cout << "[server conn#" << conn_id << " stream#" << stream_no << "] recv sid=" << quic_stream.id()
                          << " bytes=" << chunk.size() << "\n";
            } else {
                std::cout.write(chunk.get(), static_cast<std::streamsize>(chunk.size()));
                if (chunk.size() && chunk.get()[chunk.size() - 1] != '\n') {
                    std::cout << "\n";
                }
            }
            std::cout.flush();
            co_await output.write(chunk.get(), chunk.size());
            co_await output.flush();
        }
        co_await output.close();
        co_await input.close();
    } catch (const quic_error& e) {
        if (e.code() != quic_error::closed) {
            std::cerr << "[server conn#" << conn_id << " stream#" << stream_no << "] stream error: " << e.what() << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[server conn#" << conn_id << " stream#" << stream_no << "] stream exception: " << e.what() << "\n";
    }
}

static future<> handle_session(connection session, bool verbose, uint64_t conn_id) {
    gate streams;
    uint64_t next_stream_no = 1;
    try {
        while (session.is_open()) {
            auto quic_stream = co_await session.accept_stream();
            auto stream_no = next_stream_no++;
            if (verbose) {
                std::cout << "[server conn#" << conn_id << "] accepted stream sid=" << quic_stream.id() << "\n";
                std::cout.flush();
            }
            (void)with_gate(streams, [quic_stream = std::move(quic_stream), verbose, conn_id, stream_no]() mutable {
                return handle_stream(std::move(quic_stream), verbose, conn_id, stream_no);
            }).handle_exception([](std::exception_ptr ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    std::cerr << "[server] stream task failed: " << e.what() << "\n";
                }
            }).or_terminate();
        }
    } catch (const quic_error& e) {
        if (e.code() != quic_error::closed) {
            std::cerr << "[server conn#" << conn_id << "] connection error: " << e.what() << "\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[server conn#" << conn_id << "] connection exception: " << e.what() << "\n";
    }

    try {
        co_await session.close();
    } catch (...) {
    }
    try {
        co_await streams.close();
    } catch (...) {
    }
}

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
      ("address", bpo::value<std::string>()->default_value("::1"), "Server IP address")
      ("port", bpo::value<uint16_t>()->default_value(4444), "Server UDP port")
      ("crt", bpo::value<std::string>()->default_value("server.crt"), "PEM certificate file")
      ("key,k", bpo::value<std::string>()->default_value("server.key"), "PEM key file")
      ("verbose,v", bpo::value<bool>()->default_value(false)->implicit_value(true), "Verbose logging");

    return app.run(argc, argv, [&app]() -> future<int> {
        sharded_quic_server server;
        std::exception_ptr error;
        bool verbose = false;

        try {
            auto&& cfg = app.configuration();
            auto address = cfg["address"].as<std::string>();
            auto port = cfg["port"].as<uint16_t>();
            auto crt = cfg["crt"].as<std::string>();
            auto key = cfg["key"].as<std::string>();
            verbose = cfg["verbose"].as<bool>();

            quic_server_config server_cfg;
            server_cfg.listen_address = parse_ip_address(address, port);
            server_cfg.crt_file = crt;
            server_cfg.key_file = key;

            co_await server.start(std::move(server_cfg));
            co_await server.serve([verbose] {
                return [verbose] (connection session) mutable -> future<> {
                    static thread_local uint64_t next_conn_id = 1;
                    auto conn_id = (static_cast<uint64_t>(this_shard_id()) << 32) | next_conn_id++;
                    if (verbose) {
                        std::cout << "[server shard#" << this_shard_id() << " conn#" << conn_id << "] accepted\n";
                        std::cout.flush();
                    }
                    co_await handle_session(std::move(session), verbose, conn_id);
                };
            });

            std::cout << "QUIC server listening on " << server.local_address() << "\n";
            std::cout.flush();

            seastar_apps_lib::stop_signal stop_signal;
            co_await stop_signal.wait();
            std::cout << "[server] SIGINT received, disconnecting clients...\n";
            std::cout.flush();
        } catch (...) {
            error = std::current_exception();
        }

        try {
            co_await server.stop();
        } catch (...) {
            if (!error) {
                error = std::current_exception();
            }
        }

        if (error) {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& e) {
                std::cerr << "[server] fatal: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[server] fatal: unknown exception\n";
            }
            co_return 1;
        }
        co_return 0;
    });
}
