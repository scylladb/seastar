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

// Runs a small "cluster" of Seastar applications inside a single process.
//
// Each node is a full app_template with its own reactor threads, running on
// its own std::thread. Every shard of every node listens on a per-node TCP
// port, and every shard of every node pings every other node over loopback.
// Once all nodes have finished pinging, the main thread uses each node's
// alien::instance to tell it to shut down.
//
// Options not recognized by the demo itself are forwarded to every node, so
// e.g. `multi_node_demo --nodes 4 --smp 3 --memory 512M --overprovisioned`
// works. See `multi_node_demo --help` for details.

#include <seastar/core/alien.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/sleep.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>
#include <boost/program_options.hpp>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <array>
#include <iostream>
#include <future>
#include <latch>
#include <ranges>
#include <string>
#include <thread>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;
namespace bpo = boost::program_options;

static logger mlog("multi_node");

static socket_address node_address(uint16_t base_port, unsigned node) {
    return socket_address(ipv4_addr("127.0.0.1", base_port + node));
}

// Reads one '\n'-terminated line (without the terminator).
static future<sstring> read_line(input_stream<char>& in) {
    sstring line;
    while (true) {
        auto buf = co_await in.read();
        if (buf.empty()) {
            co_return line;
        }
        auto view = std::string_view(buf.get(), buf.size());
        if (auto nl = view.find('\n'); nl != view.npos) {
            line.append(view.data(), nl);
            co_return line;
        }
        line.append(view.data(), view.size());
    }
}

// Answers each "PING ..." line with a "PONG ..." line identifying this
// node and shard. One instance per shard.
class ping_server {
    unsigned _node;
    server_socket _listener;
    gate _gate;
public:
    explicit ping_server(unsigned node) : _node(node) {}

    future<> listen(socket_address addr) {
        listen_options lo;
        lo.reuse_address = true;
        _listener = seastar::listen(addr, lo);
        // Runs in the background until stop() aborts the accept.
        (void)with_gate(_gate, [this] { return accept_loop(); });
        return make_ready_future();
    }

    future<> stop() {
        _listener.abort_accept();
        return _gate.close();
    }
private:
    future<> accept_loop() {
        while (true) {
            accept_result ar;
            try {
                ar = co_await _listener.accept();
            } catch (...) {
                co_return; // aborted by stop()
            }
            (void)with_gate(_gate, [this, s = std::move(ar.connection)] () mutable {
                return handle(std::move(s));
            });
        }
    }

    future<> handle(connected_socket s) {
        auto in = s.input();
        auto out = s.output();
        std::exception_ptr ex;
        try {
            auto ping = co_await read_line(in);
            co_await out.write(fmt::format("PONG from node {} shard {} (re: {})\n", _node, this_shard_id(), ping));
            co_await out.flush();
        } catch (...) {
            ex = std::current_exception();
        }
        co_await out.close();
        co_await in.close();
        if (ex) {
            mlog.warn("node {}: connection failed: {}", _node, ex);
        }
    }
};

// Pings `peer` from the current shard, retrying while the peer is still
// starting up and not listening yet.
static future<> ping(unsigned self, unsigned peer, uint16_t base_port) {
    auto addr = node_address(base_port, peer);
    connected_socket s;
    for (unsigned attempt = 0; ; ++attempt) {
        try {
            s = co_await connect(addr);
            break;
        } catch (const std::system_error& e) {
            if (e.code().value() != ECONNREFUSED || attempt == 500) {
                throw;
            }
        }
        co_await sleep(10ms);
    }
    auto in = s.input();
    auto out = s.output();
    co_await out.write(fmt::format("PING from node {} shard {}\n", self, this_shard_id()));
    co_await out.flush();
    auto pong = co_await read_line(in);
    mlog.info("node {}: {}", self, pong);
    co_await out.close();
    co_await in.close();
}

// Coordination between the main thread and one node.
struct node_control {
    // Fulfilled by the node once it listens; carries its alien instance
    // and the promise the main thread fulfills (via alien) to stop it.
    std::promise<std::pair<alien::instance*, promise<>*>> started;
    bool has_started = false;
    int exit_code = 0;
};

// The body of one node, running on its shard 0.
static future<> node_main(unsigned id, unsigned nodes, uint16_t base_port, alien::instance& alien,
        node_control& ctl, std::latch& all_pinged) {
    sharded<ping_server> server;
    co_await server.start(id);
    co_await server.invoke_on_all(&ping_server::listen, node_address(base_port, id));
    mlog.info("node {}: listening on {} with {} shards", id, node_address(base_port, id), this_smp_shard_count());

    promise<> stop_requested;
    auto stopped = stop_requested.get_future();
    ctl.started.set_value({&alien, &stop_requested});
    ctl.has_started = true;

    std::exception_ptr ex;
    try {
        co_await smp::invoke_on_all([&] {
            auto peers = std::views::iota(0u, nodes) | std::views::filter([&] (unsigned n) { return n != id; });
            return parallel_for_each(peers, [&] (unsigned peer) {
                return ping(id, peer, base_port);
            });
        });
    } catch (...) {
        ex = std::current_exception();
    }
    all_pinged.count_down();
    // Keep serving until every node is done pinging.
    co_await std::move(stopped);
    co_await server.stop();
    if (ex) {
        std::rethrow_exception(ex);
    }
    mlog.info("node {}: done", id);
}

static int run_node(unsigned id, unsigned nodes, uint16_t base_port, std::vector<std::string> args,
        node_control& ctl, std::latch& all_pinged) {
    args.insert(args.begin(), fmt::format("node{}", id));
    auto argv = args | std::views::transform([] (std::string& a) { return a.data(); }) | std::ranges::to<std::vector<char*>>();
    argv.push_back(nullptr);

    app_template::config cfg;
    cfg.name = fmt::format("node{}", id);
    // Signals are process-wide; the main thread shuts the nodes down.
    cfg.auto_handle_sigint_sigterm = false;
    app_template app(std::move(cfg));

    return app.run(argv.size() - 1, argv.data(), [&] {
        return node_main(id, nodes, base_port, app.alien(), ctl, all_pinged);
    });
}

static constexpr auto description = R"(multi_node_demo - run a small Seastar cluster inside a single process

Starts several independent Seastar applications ("nodes") in this process.
Each node has its own app_template, reactor threads (shards) and memory, and
runs on its own std::thread; nodes share nothing except the process.

What happens:
  1. Node N listens on 127.0.0.1:<base-port + N> on every one of its shards.
  2. Every shard of every node connects to every other node and sends a PING.
     The accepting shard replies with a PONG naming its node and shard, and
     the sender logs it ("node 1: PONG from node 0 shard 1 ...").
  3. Once all nodes have pinged all their peers, the main thread tells each
     node to stop by sending it a message through its alien::instance.
  4. The process exits with 0 if every node exited with 0.

Expect one "listening" line per node, nodes * (nodes - 1) * smp "PONG" lines,
and one "done" line per node.

Usage:
  multi_node_demo [demo options] [seastar options]

Any option not listed below is a Seastar option and is passed unchanged to
every node (see --help-seastar). If none is given, each node gets
  --smp 2 --memory 256M --overprovisioned
When passing your own, keep --memory and --overprovisioned: without them,
each node would try to take most of the machine's memory, and all nodes would
pin their shards to the same CPUs.

Examples:
  multi_node_demo
      3 nodes with 2 shards each
  multi_node_demo --nodes 5 --smp 4 --memory 512M --overprovisioned
      5 nodes with 4 shards each
  multi_node_demo --base-port 20000
      use ports 20000.. if 10000.. are taken
  multi_node_demo --smp 2 --memory 256M --overprovisioned --default-log-level warn --logger-log-level multi_node=info
      show only the demo's own messages (logging options apply to every node)
)";

int main(int ac, char** av) {
    bpo::options_description desc("Demo options");
    desc.add_options()
        ("help,h", "show this help message")
        ("help-seastar", "show the Seastar options accepted by each node")
        ("nodes", bpo::value<unsigned>()->default_value(3), "number of nodes to start")
        ("base-port", bpo::value<uint16_t>()->default_value(10000), "node N listens on 127.0.0.1:<base-port + N>")
        ;
    bpo::variables_map vm;
    bpo::parsed_options parsed(&desc);
    try {
        parsed = bpo::command_line_parser(ac, av).options(desc).allow_unregistered().run();
        bpo::store(parsed, vm);
        bpo::notify(vm);
    } catch (const bpo::error& e) {
        fmt::print(std::cerr, "error: {}\n\nTry --help.\n", e.what());
        return 2;
    }
    if (vm.count("help")) {
        std::cout << description << "\n" << desc << "\n";
        return 0;
    }
    if (vm.count("help-seastar")) {
        std::string argv0 = av[0], help = "--help-seastar";
        std::array<char*, 3> argv = {argv0.data(), help.data(), nullptr};
        return app_template().run(2, argv.data(), [] { return make_ready_future(); });
    }
    auto nodes = vm["nodes"].as<unsigned>();
    auto base_port = vm["base-port"].as<uint16_t>();
    auto node_args = bpo::collect_unrecognized(parsed.options, bpo::include_positional);
    if (node_args.empty()) {
        node_args = {"--smp", "2", "--memory", "256M", "--overprovisioned"};
    }

    std::vector<node_control> ctls(nodes);
    std::latch all_pinged(nodes);
    std::vector<std::thread> threads;
    for (unsigned id = 0; id < nodes; ++id) {
        threads.emplace_back([&, id] {
            auto& ctl = ctls[id];
            ctl.exit_code = run_node(id, nodes, base_port, node_args, ctl, all_pinged);
            if (!ctl.has_started) {
                // Failed during startup; don't leave the main thread waiting.
                ctl.started.set_value({nullptr, nullptr});
                all_pinged.count_down();
            }
        });
    }

    // Wait for all nodes to start, then for all of them to finish pinging.
    auto started = ctls | std::views::transform([] (node_control& c) {
        return c.started.get_future().get();
    }) | std::ranges::to<std::vector>();
    all_pinged.wait();
    for (auto [alien, stop] : started) {
        if (alien) {
            alien::run_on(*alien, 0, [stop] () noexcept { stop->set_value(); });
        }
    }

    for (auto& t : threads) {
        t.join();
    }
    auto exit_codes = ctls | std::views::transform(&node_control::exit_code);
    fmt::print("all nodes exited, exit codes: {}\n", exit_codes);
    return std::ranges::all_of(exit_codes, [] (int c) { return c == 0; }) ? 0 : 1;
}
