// Shoal, version 6: a Redis-compatible key-value server on a single shard.
#include "commands.hh"
#include "pubsub.hh"
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/semaphore.hh>
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
    size_t max_clients;
    size_t max_request_memory;
    size_t pubsub_max_pending;
    size_t pubsub_max_total_pending;
};

// snippet: limits
// The limits that all the connections share.
struct limits {
    semaphore clients;
    semaphore request_memory;
    size_t request_memory_limit;
};
// end snippet: limits

// snippet: serve
// Reads and executes a client's commands, until the client closes the
// connection.
future<> serve(store& db, pubsub& ps, limits& lim, client& c,
               input_stream<char>& in, output_stream<char>& out,
               abort_source& hung_up) {
    request_reader reader(in, lim.request_memory, lim.request_memory_limit);
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
                co_await execute(db, *req, out, hung_up);
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
// Serves a connection. It holds one of the units of the limit on clients
// until it ends.
future<> handle_connection(store& db, pubsub& ps, limits& lim,
                           connected_socket s, semaphore_units<> admitted) {
    // Send each reply as soon as it's flushed (see the text).
    s.set_nodelay(true);
    auto in = s.input();
    auto out = s.output();
    client c(s, out);
    // Learn when the client hangs up, even while we aren't reading from it,
    // so that a command that waits, such as BLPOP, can stop waiting.
    abort_source hung_up;
    auto hangup = s.wait_input_shutdown().then([&hung_up] {
        hung_up.request_abort();
    });
    std::exception_ptr failure;
    try {
        co_await serve(db, ps, lim, c, in, out, hung_up);
    } catch (...) {
        failure = std::current_exception();
    }
    // However serve() ended, stop sending messages to this client, and wait
    // for those already sent, before c and out are destroyed.
    ps.forget(c);
    try {
        co_await c.close();
        co_await out.close();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    // Shutting down our side of the input ends the wait for the client's.
    s.shutdown_input();
    co_await std::move(hangup);
    if (failure) {
        co_await coroutine::return_exception_ptr(std::move(failure));
    }
}
// end snippet: handle-connection

// snippet: refuse
// Tells a client that there are too many clients, and closes its connection.
future<> refuse(connected_socket s) {
    auto out = s.output();
    co_await out.write(error_reply("max number of clients reached"));
    co_await out.flush();
    co_await out.close();
}
// end snippet: refuse

// snippet: shoal-main
future<> shoal_main(options opts) {
    store db;
    pubsub ps(opts.pubsub_max_pending, opts.pubsub_max_total_pending);
    limits lim{
        .clients = semaphore(opts.max_clients),
        .request_memory = semaphore(opts.max_request_memory),
        .request_memory_limit = opts.max_request_memory,
    };
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
        auto admitted = try_get_units(lim.clients, 1);
        if (!admitted) {
            (void)refuse(std::move(socket)).handle_exception(
                [] (std::exception_ptr) {});
            continue;
        }
        (void)handle_connection(db, ps, lim, std::move(socket),
                                std::move(*admitted)).handle_exception(
            [address] (std::exception_ptr e) {
                shoal_log.warn("connection with {} failed: {}", address,
                               formattable(e));
            });
    }
}
// end snippet: shoal-main

// An option given as a number of bytes, or, if it's 0, as a fraction of
// the shard's memory.
size_t memory_option(size_t option, size_t fraction) {
    return option ? option : memory::stats().total_memory() / fraction;
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
        ("maxclients", bpo::value<size_t>()->default_value(10000),
         "refuse connections beyond this many clients")
        ("max-request-memory", bpo::value<size_t>()->default_value(0),
         "the most memory that the large arguments of the requests being "
         "read may use (default: a quarter of the shard's memory)")
        ("pubsub-max-pending", bpo::value<size_t>()->default_value(32 << 20),
         "disconnect a subscriber whose unsent messages use more than this "
         "many bytes")
        ("pubsub-max-total-pending", bpo::value<size_t>()->default_value(0),
         "when all subscribers' unsent messages use more than this many "
         "bytes, publishers wait (default: a quarter of the shard's memory)");
    return app.run(argc, argv, [&app] {
        auto& config = app.configuration();
        return shoal_main(options{
            .port = config["port"].as<uint16_t>(),
            .max_clients = config["maxclients"].as<size_t>(),
            .max_request_memory = memory_option(
                config["max-request-memory"].as<size_t>(), 4),
            .pubsub_max_pending = config["pubsub-max-pending"].as<size_t>(),
            .pubsub_max_total_pending = memory_option(
                config["pubsub-max-total-pending"].as<size_t>(), 4),
        });
    });
}
