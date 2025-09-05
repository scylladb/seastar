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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#include <seastar/http/response_parser.hh>
#include <seastar/net/api.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/print.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/semaphore.hh>
#include <chrono>

using namespace seastar;

template <typename... Args>
void http_debug(const char* fmt, Args&&... args) {
#if HTTP_DEBUG
    print(fmt, std::forward<Args>(args)...);
#endif
}

class http_client {
private:
    unsigned _duration;
    unsigned _conn_per_core;
    unsigned _reqs_per_conn;
    std::vector<connected_socket> _sockets;
    semaphore _conn_connected{0};
    semaphore _conn_finished{0};
    timer<> _run_timer;
    bool _timer_based;
    bool _timer_done{false};
    uint64_t _total_reqs{0};
public:
    http_client(unsigned duration, unsigned total_conn, unsigned reqs_per_conn)
        : _duration(duration)
        , _conn_per_core(total_conn / smp::count)
        , _reqs_per_conn(reqs_per_conn)
        , _run_timer([this] { _timer_done = true; })
        , _timer_based(reqs_per_conn == 0) {
    }

    class connection {
    private:
        connected_socket _fd;
        input_stream<char> _read_buf;
        output_stream<char> _write_buf;
        http_response_parser _parser;
        http_client* _http_client;
        uint64_t _nr_done{0};
    public:
        connection(connected_socket&& fd, http_client* client)
            : _fd(std::move(fd))
            , _read_buf(_fd.input())
            , _write_buf(_fd.output())
            , _http_client(client){
        }

        uint64_t nr_done() {
            return _nr_done;
        }

        future<> do_req() {
            return _write_buf.write("GET / HTTP/1.1\r\nHost: 127.0.0.1:10000\r\n\r\n").then([this] {
                return _write_buf.flush();
            }).then([this] {
                _parser.init();
                return _read_buf.consume(_parser).then([this] {
                    // Read HTTP response header first
                    if (_parser.eof()) {
                        return make_ready_future<>();
                    }
                    auto _rsp = _parser.get_parsed_response();
                    auto it = _rsp->_headers.find("Content-Length");
                    if (it == _rsp->_headers.end()) {
                        fmt::print("Error: HTTP response does not contain: Content-Length\n");
                        return make_ready_future<>();
                    }
                    auto content_len = std::stoi(it->second);
                    http_debug("Content-Length = %d\n", content_len);
                    // Read HTTP response body
                    return _read_buf.read_exactly(content_len).then([this] (temporary_buffer<char> buf) {
                        _nr_done++;
                        http_debug("%s\n", buf.get());
                        if (_http_client->done(_nr_done)) {
                            return make_ready_future();
                        } else {
                            return do_req();
                        }
                    });
                });
            });
        }
    };

    future<uint64_t> total_reqs() {
        fmt::print("Requests on cpu {:2d}: {:d}\n", this_shard_id(), _total_reqs);
        return make_ready_future<uint64_t>(_total_reqs);
    }

    bool done(uint64_t nr_done) {
        if (_timer_based) {
            return _timer_done;
        } else {
            return nr_done >= _reqs_per_conn;
        }
    }

    future<> connect(ipv4_addr server_addr) {
        // Establish all the TCP connections first
        for (unsigned i = 0; i < _conn_per_core; i++) {
            // Connect in the background, signal _conn_connected when done.
            (void)seastar::connect(make_ipv4_address(server_addr)).then([this] (connected_socket fd) {
                _sockets.push_back(std::move(fd));
                http_debug("Established connection %6d on cpu %3d\n", _conn_connected.current(), this_shard_id());
                _conn_connected.signal();
            }).or_terminate();
        }
        return _conn_connected.wait(_conn_per_core);
    }

    future<> run() {
        // All connected, start HTTP request
        http_debug("Established all %6d tcp connections on cpu %3d\n", _conn_per_core, this_shard_id());
        if (_timer_based) {
            _run_timer.arm(std::chrono::seconds(_duration));
        }
        for (auto&& fd : _sockets) {
            auto conn = new connection(std::move(fd), this);
            // Run in the background, signal _conn_finished when done.
            (void)conn->do_req().then_wrapped([this, conn] (auto&& f) {
                http_debug("Finished connection %6d on cpu %3d\n", _conn_finished.current(), this_shard_id());
                _total_reqs += conn->nr_done();
                _conn_finished.signal();
                delete conn;
                // FIXME: should _conn_finished.signal be called only after this?
                // nothing seems to synchronize with this background work.
                try {
                    f.get();
                } catch (std::exception& ex) {
                    fmt::print("http request error: {}\n", ex.what());
                }
            });
        }

        // All finished
        return _conn_finished.wait(_conn_per_core);
    }
    future<> stop() {
        return make_ready_future();
    }
};

namespace bpo = boost::program_options;

int main(int ac, char** av) {
    app_template::config app_cfg;
    app_cfg.auto_handle_sigint_sigterm = false;
    app_template app(std::move(app_cfg));

    app.add_options()
        ("server,s", bpo::value<std::string>()->default_value("192.168.66.100:10000"), "Server address")
        ("conn,c", bpo::value<unsigned>()->default_value(100), "total connections")
        ("reqs,r", bpo::value<unsigned>()->default_value(0), "reqs per connection")
        ("duration,d", bpo::value<unsigned>()->default_value(10), "duration of the test in seconds)");

    return app.run(ac, av, [&app] () -> future<int> {
        auto& config = app.configuration();
        auto server = config["server"].as<std::string>();
        auto reqs_per_conn = config["reqs"].as<unsigned>();
        auto total_conn= config["conn"].as<unsigned>();
        auto duration = config["duration"].as<unsigned>();

        if (total_conn % smp::count != 0) {
            fmt::print("Error: conn needs to be n * cpu_nr\n");
            return make_ready_future<int>(-1);
        }

        auto http_clients = new sharded<http_client>;

        // Start http requests on all the cores
        auto started = steady_clock_type::now();
        fmt::print("========== http_client ============\n");
        fmt::print("Server: {}\n", server);
        fmt::print("Connections: {:d}\n", total_conn);
        fmt::print("Requests/connection: {}\n", reqs_per_conn == 0 ? "dynamic (timer based)" : std::to_string(reqs_per_conn));
        return http_clients->start(std::move(duration), std::move(total_conn), std::move(reqs_per_conn)).then([http_clients, server] {
            return http_clients->invoke_on_all(&http_client::connect, ipv4_addr{server});
        }).then([http_clients] {
            return http_clients->invoke_on_all(&http_client::run);
        }).then([http_clients] {
            return http_clients->map_reduce(adder<uint64_t>(), &http_client::total_reqs);
        }).then([http_clients, started] (auto total_reqs) {
           // All the http requests are finished
           auto finished = steady_clock_type::now();
           auto elapsed = finished - started;
           auto secs = static_cast<double>(elapsed.count() / 1000000000.0);
           fmt::print("Total cpus: {:d}\n", smp::count);
           fmt::print("Total requests: {:d}\n", total_reqs);
           fmt::print("Total time: {:f}\n", secs);
           fmt::print("Requests/sec: {:f}\n", static_cast<double>(total_reqs) / secs);
           fmt::print("==========     done     ============\n");
           return http_clients->stop().then([http_clients] {
               // FIXME: If we call engine().exit(0) here to exit when
               // requests are done. The tcp connection will not be closed
               // properly, becasue we exit too earily and the FIN packets are
               // not exchanged.
                delete http_clients;
                return make_ready_future<int>(0);
           });
        });
    });
}
