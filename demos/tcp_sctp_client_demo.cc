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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include <iostream>
#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/print.hh>
#include <seastar/core/units.hh>

using namespace seastar;
using namespace net;
using namespace std::chrono_literals;

static int rx_msg_size = 4_KiB;
static int tx_msg_total_size = 100_MiB;
static int tx_msg_size = 4_KiB;
static int tx_msg_nr = tx_msg_total_size / tx_msg_size;
static std::string str_txbuf(tx_msg_size, 'X');

class client;
sharded<client> clients;

transport protocol = transport::TCP;

class client {
private:
    static constexpr unsigned _pings_per_connection = 10000;
    unsigned _total_pings;
    unsigned _concurrent_connections;
    ipv4_addr _server_addr;
    std::string _test;
    lowres_clock::time_point _earliest_started = lowres_clock::time_point::max();
    lowres_clock::time_point _latest_finished = lowres_clock::time_point::min();
    size_t _processed_bytes;
    unsigned _num_reported;
public:
    class connection {
        connected_socket _fd;
        input_stream<char> _read_buf;
        output_stream<char> _write_buf;
        size_t _bytes_read = 0;
        size_t _bytes_write = 0;
    public:
        connection(connected_socket&& fd)
            : _fd(std::move(fd))
            , _read_buf(_fd.input())
            , _write_buf(_fd.output()) {}

        future<> do_read() {
            return _read_buf.read_exactly(rx_msg_size).then([this] (temporary_buffer<char> buf) {
                _bytes_read += buf.size();
                if (buf.size() == 0) {
                    return make_ready_future();
                } else {
                    return do_read();
                }
            });
        }

        future<> do_write(int end) {
            if (end == 0) {
                return make_ready_future();
            }
            return _write_buf.write(str_txbuf).then([this] {
                _bytes_write += tx_msg_size;
                return _write_buf.flush();
            }).then([this, end] {
                return do_write(end - 1);
            });
        }

        future<> ping(int times) {
            return _write_buf.write("ping").then([this] {
                return _write_buf.flush();
            }).then([this, times] {
                return _read_buf.read_exactly(4).then([this, times] (temporary_buffer<char> buf) {
                    if (buf.size() != 4) {
                        fmt::print(std::cerr, "illegal packet received: {}\n", buf.size());
                        return make_ready_future();
                    }
                    auto str = std::string(buf.get(), buf.size());
                    if (str != "pong") {
                        fmt::print(std::cerr, "illegal packet received: {}\n", buf.size());
                        return make_ready_future();
                    }
                    if (times > 0) {
                        return ping(times - 1);
                    } else {
                        return make_ready_future();
                    }
                });
            });
        }

        future<size_t> rxrx() {
            return _write_buf.write("rxrx").then([this] {
                return _write_buf.flush();
            }).then([this] {
                return do_write(tx_msg_nr).then([this] {
                    return _write_buf.close();
                }).then([this] {
                    return make_ready_future<size_t>(_bytes_write);
                });
            });
        }

        future<size_t> txtx() {
            return _write_buf.write("txtx").then([this] {
                return _write_buf.flush();
            }).then([this] {
                return do_read().then([this] {
                    return make_ready_future<size_t>(_bytes_read);
                });
            });
        }
    };

    future<> ping_test(connection *conn) {
        auto started = lowres_clock::now();
        return conn->ping(_pings_per_connection).then([started] {
            auto finished = lowres_clock::now();
            (void)clients.invoke_on(0, &client::ping_report, started, finished);
        });
    }

    future<> rxrx_test(connection *conn) {
        auto started = lowres_clock::now();
        return conn->rxrx().then([started] (size_t bytes) {
            auto finished = lowres_clock::now();
            (void)clients.invoke_on(0, &client::rxtx_report, started, finished, bytes);
        });
    }

    future<> txtx_test(connection *conn) {
        auto started = lowres_clock::now();
        return conn->txtx().then([started] (size_t bytes) {
            auto finished = lowres_clock::now();
            (void)clients.invoke_on(0, &client::rxtx_report, started, finished, bytes);
        });
    }

    void ping_report(lowres_clock::time_point started, lowres_clock::time_point finished) {
        if (_earliest_started > started)
            _earliest_started = started;
        if (_latest_finished < finished)
            _latest_finished = finished;
        if (++_num_reported == _concurrent_connections) {
            auto elapsed = _latest_finished - _earliest_started;
            auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            auto secs = static_cast<double>(usecs) / static_cast<double>(1000 * 1000);
            fmt::print(std::cout, "========== ping ============\n");
            fmt::print(std::cout, "Server: {}\n", _server_addr);
            fmt::print(std::cout,"Connections: {}\n", _concurrent_connections);
            fmt::print(std::cout, "Total PingPong: {}\n", _total_pings);
            fmt::print(std::cout, "Total Time(Secs): {}\n", secs);
            fmt::print(std::cout, "Requests/Sec: {}\n",
                static_cast<double>(_total_pings) / secs);
            (void)clients.stop().then([] {
                engine().exit(0);
            });
        }
    }

    void rxtx_report(lowres_clock::time_point started, lowres_clock::time_point finished, size_t bytes) {
        if (_earliest_started > started)
            _earliest_started = started;
        if (_latest_finished < finished)
            _latest_finished = finished;
        _processed_bytes += bytes;
        if (++_num_reported == _concurrent_connections) {
            auto elapsed = _latest_finished - _earliest_started;
            auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            auto secs = static_cast<double>(usecs) / static_cast<double>(1000 * 1000);
            fmt::print(std::cout, "========== {} ============\n", _test);
            fmt::print(std::cout, "Server: {}\n", _server_addr);
            fmt::print(std::cout, "Connections: {}\n", _concurrent_connections);
            fmt::print(std::cout, "Bytes Received(MiB): {}\n", _processed_bytes / 1_MiB);
            fmt::print(std::cout, "Total Time(Secs): {}\n", secs);
            fmt::print(std::cout, "Bandwidth(Gbits/Sec): {}\n",
                static_cast<double>((_processed_bytes * 8)) / (1000 * 1000 * 1000) / secs);
            (void)clients.stop().then([] {
                engine().exit(0);
            });
        }
    }

    future<> start(ipv4_addr server_addr, std::string test, unsigned ncon) {
        _server_addr = server_addr;
        _concurrent_connections = ncon * smp::count;
        _total_pings = _pings_per_connection * _concurrent_connections;
        _test = test;

        for (unsigned i = 0; i < ncon; i++) {
            socket_address local = socket_address(::sockaddr_in{AF_INET, INADDR_ANY, {0}});
            (void)connect(make_ipv4_address(server_addr), local, protocol).then([this, test] (connected_socket fd) {
                auto conn = new connection(std::move(fd));
                (void)(this->*tests.at(test))(conn).then_wrapped([conn] (auto&& f) {
                    delete conn;
                    try {
                        f.get();
                    } catch (std::exception& ex) {
                        fmt::print(std::cerr, "request error: {}\n", ex.what());
                    }
                });
            });
        }
        return make_ready_future();
    }
    future<> stop() {
        return make_ready_future();
    }

    typedef future<> (client::*test_fn)(connection *conn);
    static const std::map<std::string, test_fn> tests;
};

namespace bpo = boost::program_options;

int main(int ac, char ** av) {
    app_template app;
    app.add_options()
        ("server", bpo::value<std::string>()->required(), "Server address")
        ("test", bpo::value<std::string>()->default_value("ping"), "test type(ping | rxrx | txtx)")
        ("conn", bpo::value<unsigned>()->default_value(16), "nr connections per cpu")
        ("proto", bpo::value<std::string>()->default_value("tcp"), "transport protocol tcp|sctp")
        ;

    return app.run_deprecated(ac, av, [&app] {
        auto&& config = app.configuration();
        auto server = config["server"].as<std::string>();
        auto test = config["test"].as<std::string>();
        auto ncon = config["conn"].as<unsigned>();
        auto proto = config["proto"].as<std::string>();

        if (proto == "tcp") {
            protocol = transport::TCP;
        } else if (proto == "sctp") {
            protocol = transport::SCTP;
        } else {
            fmt::print(std::cerr, "Error: --proto=tcp|sctp\n");
            return engine().exit(1);
        }

        if (!client::tests.count(test)) {
            fmt::print(std::cerr, "Error: -test=ping | rxrx | txtx\n");
            return engine().exit(1);
        }

        (void)clients.start().then([server, test, ncon] () {
            return clients.invoke_on_all(&client::start, ipv4_addr{server}, test, ncon);
        });
    });
}

const std::map<std::string, client::test_fn> client::tests = {
        {"ping", &client::ping_test},
        {"rxrx", &client::rxrx_test},
        {"txtx", &client::txtx_test},
};

