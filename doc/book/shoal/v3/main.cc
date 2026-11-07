// Shoal, version 3: a Redis-compatible key-value server on a single shard.
#include "commands.hh"
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/api.hh>
#include <boost/program_options.hpp>
#include <fmt/format.h>
#include <cstdint>
#include <optional>
#include <string>

namespace bpo = boost::program_options;

logger shoal_log("shoal");

// Shoal's own settings, read from the command line.
struct options {
    uint16_t port;
};

// snippet: handle-connection
// Serves one client: reads its requests, executes them on the store, and
// sends back the replies, until the client disconnects.
future<> handle_connection(store& db, connected_socket s) {
    // Send each reply as soon as it's flushed (see the text).
    s.set_nodelay(true);
    auto in = s.input();
    auto out = s.output();
    request_reader reader(in);
    std::optional<std::string> error;
    try {
        while (auto req = co_await reader.read()) {
            co_await execute(db, *req, out);
            co_await out.flush();
        }
    } catch (protocol_error& e) {
        // We can't co_await in a catch block, so only remember the error.
        error = e.what();
    }
    if (error) {
        auto message = fmt::format("Protocol error: {}", *error);
        co_await out.write(error_reply(message));
    }
    co_await out.close();
}
// end snippet: handle-connection

// snippet: shoal-main
future<> shoal_main(options opts) {
    store db;
    listen_options lo;
    lo.reuse_address = true;
    // Version 1 keeps all its data on shard 0, so it serves every client
    // there, whatever the number of shards.
    lo.lba = server_socket::load_balancing_algorithm::fixed;
    lo.fixed_cpu = 0;
    auto listener = listen(make_ipv4_address({opts.port}), lo);
    shoal_log.info("Shoal listening on port {}", opts.port);
    while (true) {
        auto [socket, address] = co_await listener.accept();
        shoal_log.debug("client {} connected", address);
        (void)handle_connection(db, std::move(socket)).handle_exception(
            [address] (std::exception_ptr e) {
                shoal_log.warn("connection with {} failed: {}", address, e);
            });
    }
}
// end snippet: shoal-main

int main(int argc, char** argv) {
    app_template::config cfg;
    cfg.name = "Shoal";
    cfg.description = "Shoal, a Redis-compatible key-value server";
    // Shoal can't yet stop cleanly, so let Ctrl-C and kill end it at once.
    cfg.auto_handle_sigint_sigterm = false;
    app_template app(std::move(cfg));
    app.add_options()
        ("port", bpo::value<uint16_t>()->default_value(6379),
         "TCP port to listen on");
    return app.run(argc, argv, [&app] {
        auto& config = app.configuration();
        return shoal_main(options{
            .port = config["port"].as<uint16_t>(),
        });
    });
}
