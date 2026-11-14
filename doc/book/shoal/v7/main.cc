// Shoal, version 7: a Redis-compatible key-value server on a single shard.
#include "commands.hh"
#include "pubsub.hh"
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/signal.hh>
#include <seastar/core/timer.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/util/defer.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/api.hh>
#include <boost/program_options.hpp>
#include <fmt/format.h>
#include <chrono>
#include <cstdint>
#include <exception>
#include <csignal>
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
    std::chrono::milliseconds shutdown_grace;
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
                           connected_socket s, semaphore_units<> admitted,
                           gate::holder connection) {
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
    // When Shoal stops, stop reading the client's requests: serve() ends
    // after those already read, and hung_up ends a BLPOP's wait.
    // snippet: stop-reading
    auto stop_reading = db.stopping.subscribe([&s] () noexcept {
        s.shutdown_input();
    });
    // end snippet: stop-reading
    // snippet: stop-writing
    // When Shoal gives up waiting, stop writing too: a write that waits for
    // a client that doesn't read fails, and so does every later one.
    auto stop_writing = db.giving_up.subscribe([&s] () noexcept {
        s.shutdown_output();
    });
    // end snippet: stop-writing
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
future<> refuse(connected_socket s, gate::holder connection) {
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
    // snippet: signals
    // Stop when the user presses Ctrl-C (SIGINT), or kill sends SIGTERM.
    // Seastar can't remove a signal handler, so before db is destroyed,
    // replace these with handlers that do nothing.
    handle_signal(SIGINT, [&db] { db.stopping.request_abort(); });
    handle_signal(SIGTERM, [&db] { db.stopping.request_abort(); });
    auto forget_signals = defer([] () noexcept {
        handle_signal(SIGINT, [] {});
        handle_signal(SIGTERM, [] {});
    });
    // end snippet: signals
    listen_options lo;
    lo.reuse_address = true;
    // Shoal keeps all its data on shard 0, so it serves every client
    // there, whatever the number of shards.
    lo.lba = server_socket::load_balancing_algorithm::fixed;
    lo.fixed_cpu = 0;
    auto listener = listen(make_ipv4_address({opts.port}), lo);
    // snippet: accept
    // Stopping ends the wait for the next client.
    auto stop_accepting = db.stopping.subscribe([&listener] () noexcept {
        listener.abort_accept();
    });
    // Every connection holds this gate, until it ends.
    gate connections;
    shoal_log.info("Shoal listening on port {}", opts.port);
    while (true) {
        auto accepted = co_await coroutine::as_future(listener.accept());
        if (db.stopping.abort_requested()) {
            accepted.ignore_ready_future();
            break;
        }
        auto [socket, address] = accepted.get();
        shoal_log.debug("client {} connected", address);
        auto admitted = try_get_units(lim.clients, 1);
        if (!admitted) {
            (void)refuse(std::move(socket), connections.hold()).handle_exception(
                [] (std::exception_ptr) {});
            continue;
        }
        (void)handle_connection(db, ps, lim, std::move(socket),
                                std::move(*admitted), connections.hold())
                .handle_exception([address] (std::exception_ptr e) {
            shoal_log.warn("connection with {} failed: {}", address,
                           formattable(e));
        });
    }
    // end snippet: accept
    // snippet: stop
    shoal_log.info("Shoal is stopping");
    // Each connection stops reading when stopping is requested, and ends
    // after the requests it has already read. Wait for all of them, but
    // not forever: after the grace period, give up on those that remain.
    // Then stop the background work, and wait for it too.
    timer<> give_up([&db, &connections] {
        shoal_log.info("giving up on the connections still open: {}",
                       connections.get_count());
        db.giving_up.request_abort();
    });
    give_up.arm(opts.shutdown_grace);
    co_await connections.close();
    give_up.cancel();
    co_await db.background.close();
    shoal_log.info("Shoal has stopped");
}
// end snippet: stop
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
         "bytes, publishers wait (default: a quarter of the shard's memory)")
        ("shutdown-grace", bpo::value<double>()->default_value(10),
         "when stopping, how many seconds to wait for the connections to "
         "finish, before closing them");
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
            .shutdown_grace = std::chrono::milliseconds(static_cast<int64_t>(
                config["shutdown-grace"].as<double>() * 1000)),
        });
    });
}
