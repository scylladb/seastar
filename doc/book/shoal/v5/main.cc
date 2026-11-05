// Shoal, version 5: a Redis-compatible key-value server on a single shard.
#include "commands.hh"
#include "pubsub.hh"
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/api.hh>
#include <boost/program_options.hpp>
#include <fmt/format.h>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace bpo = boost::program_options;

logger shoal_log("shoal");

// Shoal's own settings, read from the command line.
struct options {
    uint16_t port;
    delivery pubsub_delivery;
    size_t pubsub_max_pending;
    size_t pubsub_max_total_pending;
};

// snippet: serve
// Reads and executes a client's commands, until the client closes the
// connection.
future<> serve(store& db, pubsub& ps, client& c, input_stream<char>& in,
               output_stream<char>& out) {
    request_reader reader(in);
    std::optional<std::string> error;
    try {
        while (auto req = co_await reader.read()) {
            if (!c.channels().empty() || is_pubsub_command(*req)) {
                // Publishers may write to this client too, so its writes
                // must go through the client.
                co_await ps.execute(c, *req);
                if (c.channels().empty()) {
                    // Back to writing directly: first finish the writes
                    // of messages published before the client unsubscribed.
                    co_await c.drain();
                }
            } else {
                co_await execute(db, *req, out);
                co_await out.flush();
            }
        }
    } catch (protocol_error& e) {
        // We can't co_await in a catch block, so only remember the error.
        error = e.what();
    }
    if (error) {
        auto message = fmt::format("Protocol error: {}", *error);
        co_await c.write(error_reply(message));
    }
}
// end snippet: serve

// snippet: handle-connection
future<> handle_connection(store& db, pubsub& ps, connected_socket s) {
    // Send each reply as soon as it's flushed (see the text).
    s.set_nodelay(true);
    auto in = s.input();
    auto out = s.output();
    client c(s, out, ps.total_pending());
    std::exception_ptr failure;
    try {
        co_await serve(db, ps, c, in, out);
    } catch (...) {
        failure = std::current_exception();
    }
    // However serve() ended, stop sending messages to this client, and wait
    // for those already sent, before c and out are destroyed.
    ps.forget(c);
    try {
        co_await c.drain();
        co_await out.close();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    if (failure) {
        co_await coroutine::return_exception_ptr(std::move(failure));
    }
}
// end snippet: handle-connection

// snippet: shoal-main
future<> shoal_main(options opts) {
    store db;
    pubsub ps(opts.pubsub_delivery, opts.pubsub_max_pending,
              opts.pubsub_max_total_pending);
    listen_options lo;
    lo.reuse_address = true;
    // Shoal keeps all its data on shard 0, so it serves every client
    // there, whatever the number of shards.
    lo.lba = server_socket::load_balancing_algorithm::fixed;
    lo.fixed_cpu = 0;
    auto listener = listen(make_ipv4_address({opts.port}), lo);
    shoal_log.info("Shoal listening on port {}", opts.port);
    while (true) {
        auto [socket, address] = co_await listener.accept();
        shoal_log.debug("client {} connected", address);
        (void)handle_connection(db, ps, std::move(socket)).handle_exception(
            [address] (std::exception_ptr e) {
                shoal_log.warn("connection with {} failed: {}", address,
                               formattable(e));
            });
    }
}
// end snippet: shoal-main

size_t max_total_pending(size_t option) {
    return option ? option : memory::stats().total_memory() / 4;
}

delivery parse_delivery(std::string_view s) {
    if (s == "sequential") {
        return delivery::sequential;
    } else if (s == "parallel") {
        return delivery::parallel;
    } else if (s == "background") {
        return delivery::background;
    }
    throw std::invalid_argument(fmt::format("unknown --pubsub-delivery {}", s));
}

int main(int argc, char** argv) {
    app_template::config cfg;
    cfg.name = "Shoal";
    cfg.description = "Shoal, a Redis-compatible key-value server";
    // Shoal can't yet stop cleanly, so let Ctrl-C and kill end it at once.
    cfg.auto_handle_sigint_sigterm = false;
    app_template app(std::move(cfg));
    app.add_options()
        ("port", bpo::value<uint16_t>()->default_value(6379),
         "TCP port to listen on")
        ("pubsub-delivery", bpo::value<std::string>()->default_value("background"),
         "how PUBLISH sends messages to subscribers: sequential, parallel "
         "or background")
        ("pubsub-max-pending", bpo::value<size_t>()->default_value(32 << 20),
         "disconnect a subscriber whose unsent messages use more than this "
         "many bytes (with background delivery)")
        ("pubsub-max-total-pending", bpo::value<size_t>()->default_value(0),
         "when all subscribers' unsent messages use more than this many "
         "bytes, disconnect those that are behind (default: a quarter of "
         "the shard's memory)");
    return app.run(argc, argv, [&app] {
        auto& config = app.configuration();
        return shoal_main(options{
            .port = config["port"].as<uint16_t>(),
            .pubsub_delivery = parse_delivery(
                config["pubsub-delivery"].as<std::string>()),
            .pubsub_max_pending = config["pubsub-max-pending"].as<size_t>(),
            .pubsub_max_total_pending = max_total_pending(
                config["pubsub-max-total-pending"].as<size_t>()),
        });
    });
}
