// A burst of connections, to a server that is late to accept them: those
// that don't fit in the server's backlog wait a second.
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

// snippet: server
// Accepts n connections, but only after a delay, like a server whose shard
// is busy with other work when the burst comes.
future<> late_server(server_socket& listener, int n) {
    co_await sleep(300ms);
    std::vector<connected_socket> accepted;
    for (int i = 0; i < n; i++) {
        accepted.push_back((co_await listener.accept()).connection);
    }
}
// end snippet: server

// snippet: clients
// Opens n connections at once, and returns how long each took.
future<std::vector<double>> connect_all(socket_address server, int n) {
    std::vector<double> seconds(n);
    std::vector<connected_socket> sockets(n);
    co_await parallel_for_each(std::views::iota(0, n), [&] (int i) {
        auto start = clk::now();
        return connect(server).then([&, i, start] (connected_socket s) {
            seconds[i] = std::chrono::duration<double>(
                clk::now() - start).count();
            sockets[i] = std::move(s);
        });
    });
    co_return seconds;
}
// end snippet: clients

future<> burst(int backlog, int n) {
    // snippet: listen
    listen_options lo;
    lo.listen_backlog = backlog;
    auto listener = listen(make_ipv4_address({"127.0.0.1", 0}), lo);
    // end snippet: listen
    auto server = late_server(listener, n);
    auto seconds = co_await connect_all(listener.local_address(), n);
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
             ->default_value(300), "connections in the burst");
    return app.run(argc, argv, [&app] -> future<> {
        auto n = app.configuration()["connections"].as<int>();
        co_await burst(100, n);
        co_await burst(1000, n);
    });
}
