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
#include <unistd.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/when_any.hh>
#include <seastar/quic/quic_client.hh>

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

static std::string format_endpoint(const std::string& ip, uint16_t port) {
    if (ip.find(':') != std::string::npos) {
        return "[" + ip + "]:" + std::to_string(port);
    }
    return ip + ":" + std::to_string(port);
}

class io_shutdown final {
public:
    future<> wait() const {
        return _done.get_shared_future();
    }

    void signal() {
        if (_signaled) {
            return;
        }
        _signaled = true;
        _done.set_value();
    }

private:
    mutable shared_promise<> _done;
    bool _signaled = false;
};

static pollable_fd duplicate_stdin() {
    auto fd = ::dup(STDIN_FILENO);
    throw_system_error_on(fd == -1, "dup(stdin)");
    return pollable_fd(file_desc::from_fd(fd));
}

static future<> receive_loop(lw_shared_ptr<input_stream<char>> input, lw_shared_ptr<io_shutdown> shutdown) {
    try {
        while (true) {
            try {
                auto chunk = co_await input->read();
                if (chunk.empty()) {
                    shutdown->signal();
                    co_return;
                }
                std::cout.write(chunk.get(), static_cast<std::streamsize>(chunk.size()));
                if (chunk.size() && chunk.get()[chunk.size() - 1] != '\n') {
                    std::cout << "\n";
                }
                std::cout.flush();
            } catch (const quic_error& e) {
                if (e.code() == quic_error::closed) {
                    shutdown->signal();
                    co_return;
                }
                throw;
            }
        }
    } catch (...) {
        shutdown->signal();
        throw;
    }
}

static future<> input_loop(
  lw_shared_ptr<output_stream<char>> output,
  lw_shared_ptr<connection> session,
  lw_shared_ptr<pollable_fd> stdin_fd,
  lw_shared_ptr<io_shutdown> shutdown,
  bool verbose) {
    char buf[2048];
    while (session->is_open()) {
        auto wait_result = co_await when_any(stdin_fd->readable(), shutdown->wait());
        auto& stdin_ready = std::get<0>(wait_result.futures);
        auto& shutdown_ready = std::get<1>(wait_result.futures);

        if (shutdown_ready.available()) {
            shutdown_ready.get();
            co_return;
        }

        stdin_ready.get();
        auto n = co_await stdin_fd->read_some(buf, sizeof(buf));
        if (n == 0) {
            if (verbose) {
                std::cout << "[client] stdin EOF, closing stream output...\n";
                std::cout.flush();
            }
            shutdown->signal();
            co_await output->close();
            co_return;
        }
        if (verbose) {
            std::cout << "[client] send bytes=" << n << "\n";
            std::cout.flush();
        }
        co_await output->write(buf, n);
        co_await output->flush();
    }
}

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
      ("address", bpo::value<std::string>()->default_value("::1"), "Server IP address")
      ("port", bpo::value<uint16_t>()->default_value(4444), "Server UDP port")
      ("server-name", bpo::value<std::string>()->default_value("localhost"), "TLS server name (SNI)")
      ("ca", bpo::value<std::string>()->default_value("server.crt"), "PEM CA/certificate file used to verify the server")
      ("verbose,v", bpo::value<bool>()->default_value(false)->implicit_value(true), "Verbose logging");

    return app.run(argc, argv, [&app]() -> future<int> {
        quic_client client;
        lw_shared_ptr<connection> session;
        lw_shared_ptr<input_stream<char>> input;
        lw_shared_ptr<output_stream<char>> output;
        lw_shared_ptr<pollable_fd> stdin_fd;
        auto shutdown = make_lw_shared<io_shutdown>();
        std::exception_ptr error;

        try {
            auto&& cfg = app.configuration();
            auto address = cfg["address"].as<std::string>();
            auto port = cfg["port"].as<uint16_t>();
            auto server_name = cfg["server-name"].as<std::string>();
            auto ca_file = cfg["ca"].as<std::string>();
            auto verbose = cfg["verbose"].as<bool>();

            quic_client_config client_cfg;
            client_cfg.remote_address = parse_ip_address(address, port);
            client_cfg.server_name = server_name;
            if (!ca_file.empty()) {
                client_cfg.ca_file = ca_file;
            }

            session = make_lw_shared<connection>(co_await client.connect(std::move(client_cfg)));
            auto quic_stream = co_await session->open_stream();
            if (verbose) {
                std::cout << "[client] opened stream sid=" << quic_stream.id() << "\n";
                std::cout.flush();
            }
            input = make_lw_shared<input_stream<char>>(quic_stream.input());
            output = make_lw_shared<output_stream<char>>(quic_stream.output());
            stdin_fd = make_lw_shared<pollable_fd>(duplicate_stdin());

            if (verbose) {
                std::cout << "[client] connected to " << format_endpoint(address, port) << "\n";
                std::cout.flush();
            }

            seastar_apps_lib::stop_signal stop_signal;
            auto raced = co_await when_any(
              when_all_succeed(
                receive_loop(input, shutdown),
                input_loop(output, session, stdin_fd, shutdown, verbose))
                .discard_result(),
              stop_signal.wait().then([session, stdin_fd, shutdown]() {
                    shutdown->signal();
                    if (stdin_fd) {
                        stdin_fd->close();
                    }
                    if (session && session->is_open()) {
                        std::cout << "[client] SIGINT received, closing session...\n";
                        std::cout.flush();
                        return session->close();
                    }
                    return make_ready_future<>();
                })
                .handle_exception([](std::exception_ptr) {}));

            auto io_task = std::move(std::get<0>(raced.futures));
            auto stop_task = std::move(std::get<1>(raced.futures));
            if (!io_task.available()) {
                co_await std::move(io_task);
            } else {
                io_task.get();
            }
            if (stop_task.available()) {
                stop_task.get();
            }
        } catch (...) {
            error = std::current_exception();
        }

        shutdown->signal();
        if (stdin_fd) {
            stdin_fd->close();
        }
        if (session) {
            try {
                co_await session->close();
            } catch (...) {
            }
        }
        if (output) {
            try {
                co_await output->close();
            } catch (...) {
            }
        }
        if (input) {
            try {
                co_await input->close();
            } catch (...) {
            }
        }

        try {
            co_await client.stop();
        } catch (...) {
        }

        if (error) {
            try {
                std::rethrow_exception(error);
            } catch (const std::exception& e) {
                std::cerr << "[client] fatal: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[client] fatal: unknown exception\n";
            }
            co_return 1;
        }
        co_return 0;
    });
}
