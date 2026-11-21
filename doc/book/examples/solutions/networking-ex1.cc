// Chapter 16, exercise 1: backlog.cc, with a server that waits five
// seconds before it accepts, to look at the sockets with ss meanwhile, and
// with --one-at-a-time, clients that connect one after another.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/net/api.hh>
#include <boost/program_options.hpp>
#include <fmt/format.h>
#include <algorithm>
#include <chrono>
#include <ranges>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

// Accepts n connections, but only after a delay, like a server whose shard
// is busy with other work when the burst comes.
future<> late_server(server_socket& listener, int n) {
    co_await sleep(5s);
    std::vector<connected_socket> accepted;
    for (int i = 0; i < n; i++) {
        accepted.push_back((co_await listener.accept()).connection);
    }
}

// Opens n connections, at once or one at a time, and returns how long
// each took.
future<std::vector<double>> connect_all(socket_address server, int n,
                                        bool one_at_a_time) {
    std::vector<double> seconds(n);
    std::vector<connected_socket> sockets(n);
    co_await max_concurrent_for_each(std::views::iota(0, n),
            one_at_a_time ? 1 : n, [&] (int i) {
        auto start = clk::now();
        return connect(server).then([&, i, start] (connected_socket s) {
            seconds[i] = std::chrono::duration<double>(
                clk::now() - start).count();
            sockets[i] = std::move(s);
        });
    });
    co_return seconds;
}

future<> burst(int backlog, int n, bool one_at_a_time) {
    listen_options lo;
    lo.listen_backlog = backlog;
    auto listener = listen(make_ipv4_address({"127.0.0.1", 0}), lo);
    auto server = late_server(listener, n);
    auto seconds = co_await connect_all(listener.local_address(), n,
                                        one_at_a_time);
    co_await std::move(server);
    auto fast = std::ranges::count_if(seconds, [] (double s) {
        return s < 0.1;
    });
    auto slow = std::ranges::count_if(seconds, [] (double s) {
        return s >= 1;
    });
    fmt::print("backlog {}: {} connections in under 0.1 s, {} took a "
               "second or more (the slowest {:.2f} s)\n", backlog, fast,
               slow, std::ranges::max(seconds));
}

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
        ("connections", boost::program_options::value<int>()
             ->default_value(300), "connections in the burst")
        ("one-at-a-time", "connect one at a time");
    return app.run(argc, argv, [&app] -> future<> {
        auto& config = app.configuration();
        auto n = config["connections"].as<int>();
        co_await burst(100, n, config.count("one-at-a-time"));
    });
}
