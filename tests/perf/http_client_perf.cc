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
 * Copyright (C) 2024 ScyllaDB
 */

/*
 * The test runs http::experimental::client against minimalistic (see below) server on
 * one shard using single "in-memory" connection.
 *
 * The client sents one request at-a-time, waiting for the server response before sending
 * the next one.
 *
 * The server is a fiber that runs on top of the raw connection, reads it up until double
 * CRLF and then responds back with the "HTTP/1.1 200 OK host: test" line. So it's not
 * http::server instance, but a lightweight mock.
 *
 * The connection is net::connected_socket wrapper over seastar::queue, not Linux socket.
 */

#include <seastar/core/seastar.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/thread.hh>
#include <seastar/http/client.hh>
#include <seastar/http/request.hh>
#include <seastar/testing/linux_perf_event.hh>
#include <../../tests/unit/loopback_socket.hh>
#include <fmt/printf.h>
#include <string>

using namespace seastar;
using namespace std::chrono_literals;

class server {
    seastar::server_socket _ss;
    seastar::connected_socket _cs;
    seastar::input_stream<char> _in;
    seastar::output_stream<char> _out;
    sstring _req;

    future<> run_serve_loop() {
        while (true) {
            temporary_buffer<char> buf = co_await _in.read();
            if (buf.empty()) {
                co_return;
            }

            _req += sstring(buf.get(), buf.size());
            if (_req.ends_with("\r\n\r\n")) {
                sstring r200("HTTP/1.1 200 OK\r\nHost: test\r\n\r\n");
                co_await _out.write(r200);
                co_await _out.flush();
                _req = "";
            }
        }
    }

public:
    server(loopback_connection_factory& lcf) : _ss(lcf.get_server_socket()) {}
    future<> serve() {
        return _ss.accept().then([this] (seastar::accept_result ar) {
            _cs = std::move(ar.connection);
            _in = _cs.input();
            _out = _cs.output();
            return run_serve_loop().finally([this] {
                return when_all(_in.close(), _out.close()).discard_result();
            });
        });
    }
};

class loopback_http_factory : public http::experimental::connection_factory {
    loopback_socket_impl lsi;
public:
    explicit loopback_http_factory(loopback_connection_factory& f) : lsi(f) {}
    virtual future<connected_socket> make(abort_source* as) override {
        return lsi.connect(socket_address(ipv4_addr()), socket_address(ipv4_addr()));
    }
};

class client {
    seastar::http::experimental::client _cln;
    const unsigned _warmup_limit;
    const unsigned _limit;
    linux_perf_event _instructions;
    linux_perf_event _cpu_cycles;

    struct stats {
        std::chrono::steady_clock::time_point ts;
        uint64_t mallocs;
        uint64_t tasks;
        uint64_t instructions;
        uint64_t cpu_cycles;
    };

    stats stats_snapshot() {
        return stats {
            .ts = std::chrono::steady_clock::now(),
            .mallocs = memory::stats().mallocs(),
            .tasks = engine().get_sched_stats().tasks_processed,
            .instructions = _instructions.read(),
            .cpu_cycles = _cpu_cycles.read(),
        };
    }

    future<> make_requests(unsigned nr) {
        for (unsigned i = 0; i < nr; i++) {
            auto req = http::request::make("GET", "test", "/test");
            co_await _cln.make_request(std::move(req), [] (const http::reply& rep, input_stream<char>&& in) {
                return make_ready_future<>();
            }, http::reply::status_type::ok);
        }
    }
public:
    client(loopback_connection_factory& lcf, unsigned ops, unsigned warmup)
            : _cln(std::make_unique<loopback_http_factory>(lcf))
            , _warmup_limit(warmup)
            , _limit(ops)
            , _instructions(linux_perf_event::user_instructions_retired())
            , _cpu_cycles(linux_perf_event::user_cpu_cycles_retired())
    {}
    future<> work() {
        fmt::print("Warming up with {} requests\n", _warmup_limit);
        return make_requests(_warmup_limit).then([this] {
            fmt::print("Warmup finished, making {} requests\n", _limit);
            auto start_stats = stats_snapshot();
            _instructions.enable();
            _cpu_cycles.enable();
            return make_requests(_limit).then([this, start_stats] {
                _instructions.disable();
                _cpu_cycles.disable();
                auto end_stats = stats_snapshot();
                auto delta = std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(end_stats.ts - start_stats.ts) / _limit;
                auto allocs = double(end_stats.mallocs - start_stats.mallocs) / _limit;
                auto tasks = double(end_stats.tasks - start_stats.tasks) / _limit;
                auto insns = (end_stats.instructions - start_stats.instructions) / _limit;
                auto cycles = (end_stats.cpu_cycles - start_stats.cpu_cycles) / _limit;
                fmt::print("Made {} requests, {:.3f} usec/op, {:.1f} allocs/op, {:.1f} tasks/op, {} insns/op, {} cycles/op\n", _limit,
                        delta.count(), allocs, tasks, insns, cycles);
            });
        }).finally([this] {
            return _cln.close();
        });
    }
};

int main(int ac, char** av) {
    app_template at;
    namespace bpo = boost::program_options;
    at.add_options()
            ("total-ops", bpo::value<unsigned>()->default_value(1000000), "Total requests to make")
            ("warmup-ops", bpo::value<unsigned>()->default_value(10000), "Requests to warm up")
            ;
    return at.run(ac, av, [&at] {
        auto total_ops = at.configuration()["total-ops"].as<unsigned>();
        auto warmup_ops = at.configuration()["warmup-ops"].as<unsigned>();
        return seastar::async([total_ops, warmup_ops] {
            loopback_connection_factory lcf(1);
            server srv(lcf);
            client cln(lcf, total_ops, warmup_ops);
            when_all(srv.serve(), cln.work()).discard_result().get();
        });
    });
}
