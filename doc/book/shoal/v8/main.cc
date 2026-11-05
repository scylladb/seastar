// Shoal, version 8: a Redis-compatible key-value server that uses every
// shard.
#include "commands.hh"
#include "pubsub.hh"
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/signal.hh>
#include <seastar/core/timer.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/net/api.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/later.hh>
#include <boost/program_options.hpp>
#include <fmt/format.h>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <deque>
#include <exception>
#include <optional>
#include <string>
#include <vector>

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

// snippet: ordered-replies
// The replies to a connection's requests, which may be executed at the
// same time, but must be written in the order of the requests. They wait
// in a queue, which one coroutine, the writer, writes in order, each
// reply as soon as it's ready.
class ordered_replies {
    // The most requests of one connection executed at the same time.
    static constexpr size_t max_in_flight = 128;
    struct pending {
        future<sstring> reply;
        semaphore_units<> memory, in_flight;    // kept until it's written
    };
    output_stream<char>& _out;
    semaphore _in_flight{max_in_flight};
    std::deque<pending> _pending;               // not yet written, in order
    future<> _writing = make_ready_future<>();  // the writer, if running
public:
    explicit ordered_replies(output_stream<char>& out) : _out(out) {}
    // Waits until another request may be executed.
    future<semaphore_units<>> reserve() {
        return get_units(_in_flight, 1);
    }
    // Writes a request's reply after the previous ones. The units, of the
    // request's memory and of reserve(), are kept until it's written.
    void add(future<sstring> reply,
             semaphore_units<> memory, semaphore_units<> in_flight) {
        _pending.push_back({std::move(reply), std::move(memory),
                            std::move(in_flight)});
        if (_writing.available()) {
            // No writer is running, so start one. If the last one failed,
            // this one fails too, after only waiting for its replies.
            _writing = write(_writing.failed() ? _writing.get_exception()
                                               : nullptr);
        }
    }
    // Waits until every reply so far is written.
    future<> drain() {
        return std::exchange(_writing, make_ready_future<>());
    }
    // Whether every reply so far is written, so that the next one may be
    // written at once, without the queue. After a failure, none may.
    bool idle() const {
        return _writing.available() && !_writing.failed();
    }
private:
    future<> write(std::exception_ptr failure) {
        while (!_pending.empty()) {
            auto p = std::move(_pending.front());
            _pending.pop_front();
            try {
                // Wait for the reply even after a failure, so that no
                // request is still executing when the connection ends.
                auto reply = co_await std::move(p.reply);
                if (!failure) {
                    co_await _out.write(reply);
                    // Flush when no more replies are waiting, so that
                    // replies that are ready together are sent together.
                    if (_pending.empty()) {
                        co_await _out.flush();
                    }
                }
            } catch (...) {
                failure = failure ? failure : std::current_exception();
            }
        }
        if (failure) {
            co_await coroutine::return_exception_ptr(std::move(failure));
        }
    }
};
// end snippet: ordered-replies

// snippet: server
// What a shard needs to serve its connections. Every shard has an
// instance; shard 0's also counts the clients of all the shards.
class server : public peering_sharded_service<server> {
    sharded<store>& _db;
    sharded<pubsub>& _ps;
    options _opts;
    semaphore _request_memory;      // this shard's requests' memory
    gate _connections;              // held by this shard's connections
    size_t _clients = 0;            // on shard 0: clients of all shards
public:
    server(sharded<store>& db, sharded<pubsub>& ps, options opts)
        : _db(db), _ps(ps), _opts(opts)
        , _request_memory(opts.max_request_memory) {}
    // Accepts connections on this shard, until Shoal stops.
    future<> accept_connections();
    // Waits for this shard's connections to end.
    future<> wait_for_connections() { return _connections.close(); }
    future<> stop() {
        return _connections.is_closed() ? make_ready_future<>()
                                        : _connections.close();
    }
private:
    future<> handle_connection(connected_socket s);
    future<> serve(client& c, input_stream<char>& in,
                   output_stream<char>& out, abort_source& hung_up);
    // On shard 0: admits one more client, unless there are too many.
    bool admit_client() {
        return _clients < _opts.max_clients && ++_clients;
    }
    void release_client() { _clients--; }
};
// end snippet: server

// snippet: shard-queue
// A connection's requests to one shard. The requests that the connection
// reads together are sent together, as one batch, which the shard executes
// in order; those that come while it's executed there wait, and are the
// next batch. So the connection's requests to a shard, and to each of its
// keys, are executed in order, with one message for many requests.
class shard_queue {
    struct waiting {
        const command* cmd;
        request req;
        promise<sstring> reply;
    };
    sharded<store>& _db;
    unsigned _shard;
    std::vector<waiting> _waiting;      // the next batch
    future<> _sending = make_ready_future<>();
public:
    shard_queue(sharded<store>& db, unsigned shard) : _db(db), _shard(shard) {}
    // Sends a request to the shard, and returns its reply.
    future<sstring> add(const command* cmd, request req) {
        _waiting.push_back({cmd, std::move(req), {}});
        auto reply = _waiting.back().reply.get_future();
        if (_sending.available()) {
            _sending = send();
        }
        return reply;
    }
    // Whether no request of this connection is waiting for, or executing
    // on, this queue's shard.
    bool idle() const { return _waiting.empty() && _sending.available(); }
    // Waits until the last batch has been sent.
    future<> close() {
        return std::exchange(_sending, make_ready_future<>());
    }
private:
    // Sends batches, one at a time, until no request is waiting.
    future<> send() {
        // Before the first batch, let the connection go on reading: the
        // requests it already has, of a pipeline, join the batch.
        co_await yield();
        while (!_waiting.empty()) {
            auto batch = std::exchange(_waiting, {});
            auto replies = co_await coroutine::as_future(
                _db.invoke_on(_shard, [&batch] (store& s) {
                    std::vector<sstring> replies;
                    replies.reserve(batch.size());
                    for (auto& w : batch) {
                        replies.push_back(execute_here(s, w.cmd, w.req));
                    }
                    return replies;
                }));
            if (replies.failed()) {
                auto e = replies.get_exception();
                for (auto& w : batch) {
                    w.reply.set_exception(e);
                }
                continue;
            }
            auto r = replies.get();
            for (size_t i = 0; i < batch.size(); i++) {
                batch[i].reply.set_value(std::move(r[i]));
            }
        }
    }
};
// end snippet: shard-queue

// snippet: serve
// Reads and executes a client's commands, until the client closes the
// connection.
future<> server::serve(client& c, input_stream<char>& in,
                       output_stream<char>& out, abort_source& hung_up) {
    request_reader reader(in, _request_memory, _opts.max_request_memory);
    ordered_replies replies(out);
    auto& ps = _ps.local();
    // The connection's queue of requests to each shard. A queue's sending
    // coroutine points to it, so the vector must never move them.
    std::vector<shard_queue> queues;
    queues.reserve(this_smp_shard_count());
    for (unsigned i = 0; i < this_smp_shard_count(); i++) {
        queues.emplace_back(_db, i);
    }
    std::optional<std::string> error;
    std::exception_ptr failure;
    try {
        while (true) {
            auto req = co_await reader.read();
            if (!req) {
                break;
            }
            auto* cmd = find_command(*req);
            auto shard = batch_shard(cmd, *req);
            if (!c.channels().empty() || is_pubsub_command(*req)) {
                // As in version 7, but only after the earlier replies.
                co_await replies.drain();
                co_await ps.execute(c, *req);
                if (c.channels().empty()) {
                    co_await c.drain();
                }
            } else if (shard == this_shard_id()) {
                // A request on this shard: execute it now, without a queue
                // (this shard's requests never wait, so they stay in
                // order), and if no earlier reply is waiting, write its
                // reply now, as version 7 did. This keeps the speed of one
                // shard.
                auto reply = execute_here(_db.local(), cmd, *req);
                if (replies.idle()) {
                    co_await out.write(reply);
                    co_await out.flush();
                } else {
                    replies.add(make_ready_future<sstring>(
                                    std::move(reply)),
                                reader.take_units(),
                                co_await replies.reserve());
                }
            } else if (shard) {
                // Send it to its shard, and go on to read the next request.
                // A client that sends requests faster than they're executed
                // waits for room, and Shoal stops reading its requests.
                auto in_flight = co_await replies.reserve();
                replies.add(queues[*shard].add(cmd, std::move(*req)),
                            reader.take_units(), std::move(in_flight));
            } else {
                // Any other command is executed alone, after the earlier
                // ones, as in Redis.
                co_await replies.drain();
                auto reply = co_await execute(_db, std::move(*req), hung_up);
                if (reply) {
                    co_await out.write(*reply);
                    co_await out.flush();
                }
            }
        }
    } catch (protocol_error& e) {
        // We can't co_await in a catch block, so only remember the error.
        error = e.what();
    } catch (...) {
        failure = std::current_exception();
    }
    // However the loop ended, wait for the requests being executed, whose
    // replies are written to out, before out is closed.
    auto drained = co_await coroutine::as_future(replies.drain());
    for (auto& q : queues) {
        co_await q.close();
    }
    if (failure) {
        drained.ignore_ready_future();
        co_await coroutine::return_exception_ptr(std::move(failure));
    }
    co_await std::move(drained);
    if (error) {
        auto message = fmt::format("Protocol error: {}", *error);
        co_await c.write(error_reply(message));
    }
}
// end snippet: serve

// snippet: refuse
// Tells a client that there are too many clients.
future<> refuse(output_stream<char>& out) {
    co_await out.write(error_reply("max number of clients reached"));
    co_await out.flush();
}
// end snippet: refuse

// snippet: handle-connection
// Serves a connection, if shard 0 admits it.
future<> server::handle_connection(connected_socket s) {
    auto holder = _connections.hold();
    auto& db = _db.local();
    // Send each reply as soon as it's flushed (see chapter 4).
    s.set_nodelay(true);
    auto in = s.input();
    auto out = s.output();
    // When Shoal stops, stop reading the client's requests; when it gives
    // up waiting, stop writing too. Subscribe before the first co_await:
    // a stop requested while we wait would be missed (chapter 12). And if
    // Shoal was already stopping, stop reading at once.
    auto stop_reading = db.stopping.subscribe([&s] () noexcept {
        s.shutdown_input();
    });
    if (!stop_reading) {
        s.shutdown_input();
    }
    auto stop_writing = db.giving_up.subscribe([&s] () noexcept {
        s.shutdown_output();
    });
    // The limit on clients is for all the shards together, so shard 0
    // counts them.
    if (!co_await container().invoke_on(0, &server::admit_client)) {
        co_await refuse(out);
        co_return co_await out.close();
    }
    auto release = defer([this, &holder] () noexcept {
        // Nothing waits for this message, but the connection's hold on the
        // gate, moved here, keeps the server alive until it's done.
        (void)container().invoke_on(0, &server::release_client)
            .finally([holder = std::move(holder)] {});
    });
    client c(s, out);
    // Learn when the client hangs up, even while we aren't reading from it,
    // so that a command that waits, such as BLPOP, can stop waiting.
    abort_source hung_up;
    auto hangup = s.wait_input_shutdown().then([&hung_up] {
        hung_up.request_abort();
    });
    std::exception_ptr failure;
    try {
        co_await serve(c, in, out, hung_up);
    } catch (...) {
        failure = std::current_exception();
    }
    // However serve() ended, stop sending messages to this client, and wait
    // for those already sent, before c and out are destroyed.
    _ps.local().forget(c);
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

// snippet: accept
future<> server::accept_connections() {
    auto& db = _db.local();
    listen_options lo;
    lo.reuse_address = true;
    auto listener = listen(make_ipv4_address({_opts.port}), lo);
    // Stopping ends the wait for the next client.
    auto stop_accepting = db.stopping.subscribe([&listener] () noexcept {
        listener.abort_accept();
    });
    if (this_shard_id() == 0) {
        shoal_log.info("Shoal listening on port {}, on {} shards", _opts.port,
                       this_smp_shard_count());
    }
    while (true) {
        auto accepted = co_await coroutine::as_future(listener.accept());
        if (db.stopping.abort_requested()) {
            accepted.ignore_ready_future();
            co_return;
        }
        auto [socket, address] = accepted.get();
        shoal_log.debug("client {} connected", address);
        (void)handle_connection(std::move(socket)).handle_exception(
                [address] (std::exception_ptr e) {
            shoal_log.warn("connection with {} failed: {}", address,
                           formattable(e));
        });
    }
}
// end snippet: accept

// snippet: run
// Runs Shoal, on every shard, until it's asked to stop.
future<> run(sharded<store>& db, sharded<server>& srv, options opts) {
    // Stop when the user presses Ctrl-C (SIGINT), or kill sends SIGTERM:
    // signals arrive at shard 0, which tells the others, below. Seastar
    // can't remove a signal handler, so before db is stopped, replace these
    // with handlers that do nothing.
    handle_signal(SIGINT, [&db] { db.local().stopping.request_abort(); });
    handle_signal(SIGTERM, [&db] { db.local().stopping.request_abort(); });
    auto forget_signals = defer([] () noexcept {
        handle_signal(SIGINT, [] {});
        handle_signal(SIGTERM, [] {});
    });
    promise<> stop_requested;
    auto subscription = db.local().stopping.subscribe([&] () noexcept {
        stop_requested.set_value();
    });
    // Accept connections on every shard. If a shard can't, stop.
    auto accepting = srv.invoke_on_all(&server::accept_connections)
        .handle_exception([&db] (std::exception_ptr e) {
            db.local().stopping.request_abort();
            return make_exception_future<>(std::move(e));
        });
    co_await stop_requested.get_future();
    shoal_log.info("Shoal is stopping");
    co_await db.invoke_on_others([] (store& s) {
        s.stopping.request_abort();
    });
    co_await std::move(accepting);
    // Each connection stops reading when stopping is requested, and ends
    // after the requests it has already read. Wait for all of them, on
    // every shard, but not forever: after the grace period, give up on
    // those that remain.
    std::optional<future<>> gave_up;
    timer<> give_up([&] {
        shoal_log.info("giving up on the connections still open");
        gave_up = db.invoke_on_all([] (store& s) {
            s.giving_up.request_abort();
        });
    });
    give_up.arm(opts.shutdown_grace);
    co_await srv.invoke_on_all(&server::wait_for_connections);
    give_up.cancel();
    if (gave_up) {
        co_await std::move(*gave_up);
    }
}
// end snippet: run

// snippet: shoal-main
future<> shoal_main(options opts) {
    sharded<store> db;
    sharded<pubsub> ps;
    sharded<server> srv;
    std::exception_ptr failure;
    try {
        co_await db.start();
        co_await ps.start(opts.pubsub_max_pending,
                          opts.pubsub_max_total_pending);
        co_await srv.start(std::ref(db), std::ref(ps), opts);
        co_await run(db, srv, opts);
    } catch (...) {
        failure = std::current_exception();
    }
    // Stop the services in the opposite order: each may use the ones
    // started before it. (A service that didn't start has nothing to stop.)
    // The store's stop() waits for its background work.
    co_await srv.stop();
    co_await ps.stop();
    co_await db.stop();
    shoal_log.info("Shoal has stopped");
    if (failure) {
        co_await coroutine::return_exception_ptr(std::move(failure));
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
    app_template app(std::move(cfg));
    app.add_options()
        ("port", bpo::value<uint16_t>()->default_value(6379),
         "TCP port to listen on")
        ("maxclients", bpo::value<size_t>()->default_value(10000),
         "refuse connections beyond this many clients")
        ("max-request-memory", bpo::value<size_t>()->default_value(0),
         "the most memory that the large arguments of the requests being "
         "read on each shard may use (default: a quarter of the shard's "
         "memory)")
        ("pubsub-max-pending", bpo::value<size_t>()->default_value(32 << 20),
         "disconnect a subscriber whose unsent messages use more than this "
         "many bytes")
        ("pubsub-max-total-pending", bpo::value<size_t>()->default_value(0),
         "when the unsent messages of a shard's subscribers use more than "
         "this many bytes, publishers wait (default: a quarter of the "
         "shard's memory)")
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
