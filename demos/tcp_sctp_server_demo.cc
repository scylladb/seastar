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
 * Copyright 2014 Cloudius Systems
 */

#include <seastar/core/reactor.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/print.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/closeable.hh>
#include <vector>
#include <iostream>
#include "../apps/lib/stop_signal.hh"

using namespace seastar;

static std::string str_ping{"ping"};
static std::string str_txtx{"txtx"};
static std::string str_rxrx{"rxrx"};
static std::string str_pong{"pong"};
static std::string str_unknow{"unknow cmd"};
static int tx_msg_total_size = 100 * 1024 * 1024;
static int tx_msg_size = 4 * 1024;
static int tx_msg_nr = tx_msg_total_size / tx_msg_size;
static int rx_msg_size = 4 * 1024;
static std::string str_txbuf(tx_msg_size, 'X');
static bool enable_tcp = false;
static bool enable_sctp = false;

class tcp_server {
    std::vector<server_socket> _tcp_listeners;
    std::vector<server_socket> _sctp_listeners;
    std::optional<future<>> _tcp_task;
    std::optional<future<>> _sctp_task;

public:
    future<> listen(ipv4_addr addr) {
        if (enable_tcp) {
            listen_options lo;
            lo.proto = transport::TCP;
            lo.reuse_address = true;
            _tcp_listeners.push_back(seastar::listen(make_ipv4_address(addr), lo));
            _tcp_task = do_accepts(_tcp_listeners);
        }

        if (enable_sctp) {
            listen_options lo;
            lo.proto = transport::SCTP;
            lo.reuse_address = true;
            _sctp_listeners.push_back(seastar::listen(make_ipv4_address(addr), lo));
            _sctp_task = do_accepts(_sctp_listeners);
        }
        return make_ready_future<>();
    }

    future<> stop() {
        co_await do_stop(_tcp_listeners, _tcp_task);
        co_await do_stop(_sctp_listeners, _sctp_task);
    }

    future<> do_accepts(std::vector<server_socket>& listeners) {
        int which = listeners.size() - 1;
        // Accept in the background.
        return listeners[which].accept().then([this, &listeners] (accept_result ar) mutable {
            connected_socket fd = std::move(ar.connection);
            socket_address addr = std::move(ar.remote_address);
            auto conn = new connection(*this, std::move(fd), addr);
            (void)conn->process().then_wrapped([conn] (auto&& f) {
                delete conn;
                try {
                    f.get();
                } catch (std::exception& ex) {
                    std::cout << "request error " << ex.what() << "\n";
                }
            });
            return do_accepts(listeners);
        }).then_wrapped([] (auto&& f) {
            try {
                f.get();
            } catch (std::exception& ex) {
                std::cout << "accept failed: " << ex.what() << "\n";
            }
        });
    }

    static future<> do_stop(std::vector<server_socket>& listeners, std::optional<future<>>& task) {
        for (auto& listener : listeners) {
            listener.abort_accept();
        }
        if (auto fut = std::exchange(task, {})) {
            co_await std::move(*fut);
        }
    }

    class connection {
        connected_socket _fd;
        input_stream<char> _read_buf;
        output_stream<char> _write_buf;
    public:
        connection(tcp_server& server, connected_socket&& fd, socket_address addr)
            : _fd(std::move(fd))
            , _read_buf(_fd.input())
            , _write_buf(_fd.output()) {}
        future<> process() {
             return read();
        }
        future<> read() {
            if (_read_buf.eof()) {
                return make_ready_future();
            }
            // Expect 4 bytes cmd from client
            size_t n = 4;
            return _read_buf.read_exactly(n).then([this] (temporary_buffer<char> buf) {
                if (buf.size() == 0) {
                    return make_ready_future();
                }
                auto cmd = std::string(buf.get(), buf.size());
                // pingpong test
                if (cmd == str_ping) {
                    return _write_buf.write(str_pong).then([this] {
                        return _write_buf.flush();
                    }).then([this] {
                        return this->read();
                    });
                // server tx test
                } else if (cmd == str_txtx) {
                    return tx_test();
                // server tx test
                } else if (cmd == str_rxrx) {
                    return rx_test();
                // unknow test
                } else {
                    return _write_buf.write(str_unknow).then([this] {
                        return _write_buf.flush();
                    }).then([] {
                        return make_ready_future();
                    });
                }
            });
        }
        future<> do_write(int end) {
            if (end == 0) {
                return make_ready_future<>();
            }
            return _write_buf.write(str_txbuf).then([this] {
                return _write_buf.flush();
            }).then([this, end] {
                return do_write(end - 1);
            });
        }
        future<> tx_test() {
            return do_write(tx_msg_nr).then([this] {
                return _write_buf.close();
            }).then([] {
                return make_ready_future<>();
            });
        }
        future<> do_read() {
            return _read_buf.read_exactly(rx_msg_size).then([this] (temporary_buffer<char> buf) {
                if (buf.size() == 0) {
                    return make_ready_future();
                } else {
                    return do_read();
                }
            });
        }
        future<> rx_test() {
            return do_read().then([] {
                return make_ready_future<>();
            });
        }
    };
};

namespace bpo = boost::program_options;

int main(int ac, char** av) {
    app_template app;
    app.add_options()
        ("port", bpo::value<uint16_t>()->default_value(10000), "TCP server port")
        ("tcp", bpo::value<std::string>()->default_value("yes"), "tcp listen")
        ("sctp", bpo::value<std::string>()->default_value("no"), "sctp listen") ;
    return app.run(ac, av, [&] {
        return async([&app] {
            seastar_apps_lib::stop_signal stop_signal;

            auto&& config = app.configuration();
            uint16_t port = config["port"].as<uint16_t>();
            enable_tcp = config["tcp"].as<std::string>() == "yes";
            enable_sctp = config["sctp"].as<std::string>() == "yes";
            if (!enable_tcp && !enable_sctp) {
                fmt::print(std::cerr, "Error: no protocols enabled. Use \"--tcp yes\" and/or \"--sctp yes\" to enable\n");
                return 1;
            }
            sharded<tcp_server> server;
            server.start().get();
            auto stop_server = deferred_stop(server);
            // Start listening in the background.
            server.invoke_on_all(&tcp_server::listen, ipv4_addr{port}).get();
            fmt::print("Seastar TCP server listening on port {} ...\n", port);

            stop_signal.wait().get();
            return 0;
        });
    });
}
