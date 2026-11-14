// Shoal, version 10: a Redis-compatible key-value server that uses every
// shard.
#include "commands.hh"
#include "pubsub.hh"
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/signal.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/when_all.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/net/api.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/net/tls.hh>
#include <seastar/net/unix_address.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/later.hh>
#include <boost/program_options.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace bpo = boost::program_options;

logger shoal_log("shoal");

// Shoal's own settings, read from the command line.
struct options {
    std::vector<std::string> bind;      // addresses or names to listen on
    std::vector<net::inet_address> bind_addresses;  // bind, resolved
    uint16_t port;
    uint16_t tls_port;                  // 0: no TLS
    sstring tls_cert_file, tls_key_file, tls_ca_cert_file;
    tls::client_auth tls_auth_clients;
    sstring unix_socket;                // empty: no Unix-domain socket
    int tcp_backlog;
    std::chrono::seconds tcp_keepalive;
    sstring dir;                        // where the logs are
    bool sync_always;                   // --appendfsync always
    size_t max_clients;
    size_t max_request_memory;
    size_t max_reply_memory;
    size_t pubsub_max_pending;
    size_t pubsub_max_total_pending;
    std::chrono::milliseconds shutdown_grace;
};

// snippet: ordered-replies
// The replies to a connection's requests, which may be executed at the
// same time, but must be written in the order of the requests. They wait
// in a queue, which one coroutine, the writer, writes in order, each
// reply as soon as it's ready. Nothing here waits for the client to read
// its replies: while it doesn't, they wait in the queue, and their bytes
// are counted in the shard's reply memory, so that the connection can go
// on reading requests until that memory runs out.
class ordered_replies {
    // The most requests of one connection executed at the same time.
    static constexpr size_t max_in_flight = 128;
    struct ready_reply {
        sstring reply;
        semaphore_units<> reply_memory;         // its bytes, until written
    };
    struct pending {
        future<ready_reply> reply;
        semaphore_units<> memory;               // the request's, until written
    };
    output_stream<char>& _out;
    semaphore& _reply_memory;                   // the shard's waiting replies
    semaphore _in_flight{max_in_flight};
    std::deque<pending> _pending;               // not yet written, in order
    future<> _writing = make_ready_future<>();  // the writer, if running
public:
    ordered_replies(output_stream<char>& out, semaphore& reply_memory)
        : _out(out), _reply_memory(reply_memory) {}
    // Waits until another request may be executed.
    future<semaphore_units<>> reserve() {
        return get_units(_in_flight, 1);
    }
    // Writes a request's reply after the previous ones. The units of
    // reserve() are given back when the reply is ready, and those of the
    // request's memory when it's written. Meanwhile the ready reply's
    // bytes are taken from the reply memory, without waiting.
    void add(future<sstring> reply,
             semaphore_units<> memory, semaphore_units<> in_flight) {
        auto ready = reply.then(
                [this, in_flight = std::move(in_flight)] (sstring r) {
            // (The queue's entry is counted too, roughly.)
            auto units = consume_units(_reply_memory,
                                       sizeof(pending) + r.size());
            return ready_reply{std::move(r), std::move(units)};
        });
        _pending.push_back({std::move(ready), std::move(memory)});
        if (_writing.available()) {
            // No writer is running, so start one. If the last one failed,
            // this one fails too, after only waiting for its replies.
            _writing = write(std::move(_writing));
        }
    }
    // Writes a ready reply, if no earlier one is waiting, and returns
    // true; otherwise returns false, and the caller must add() it. If the
    // stream must wait for the client to read, the writer waits for it,
    // and the replies that come meanwhile wait in the queue.
    bool write_now(const sstring& reply) {
        if (!_writing.available() || _writing.failed()) {
            return false;
        }
        auto written = _out.write(reply).then([this] {
            return _out.flush();    // usually ready: the reactor flushes
        });
        if (!written.available() || written.failed()) {
            _writing = write(std::move(written));
        }
        return true;
    }
    // Waits while the shard's waiting replies use all its reply memory.
    future<> wait_for_memory() {
        if (_reply_memory.available_units() >= 0) {
            return make_ready_future<>();
        }
        return _reply_memory.wait(0);
    }
    // Waits until every reply so far is written.
    future<> drain() {
        return std::exchange(_writing, make_ready_future<>());
    }
private:
    // Waits for the write before it, and then writes the queue.
    future<> write(future<> before) {
        std::exception_ptr failure;
        try {
            co_await std::move(before);
        } catch (...) {
            failure = std::current_exception();
        }
        while (!_pending.empty()) {
            auto p = std::move(_pending.front());
            _pending.pop_front();
            try {
                // Wait for the reply even after a failure, so that no
                // request is still executing when the connection ends.
                auto ready = co_await std::move(p.reply);
                if (!failure) {
                    co_await _out.write(ready.reply);
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

// snippet: counting-streams
// A data source that counts the bytes it delivers from another source.
class counting_source final : public data_source_impl {
    data_source _source;
    uint64_t& _bytes;
public:
    counting_source(data_source source, uint64_t& bytes)
        : _source(std::move(source)), _bytes(bytes) {}
    future<temporary_buffer<char>> get() override {
        return _source.get().then([this] (temporary_buffer<char> buf) {
            _bytes += buf.size();
            return buf;
        });
    }
    future<> close() override {
        return _source.close();
    }
};

// A data sink that counts the bytes it passes on to another sink. The
// stream asks the sink how to buffer and flush, so it asks the other.
class counting_sink final : public data_sink_impl {
    data_sink _sink;
    uint64_t& _bytes;
public:
    counting_sink(data_sink sink, uint64_t& bytes)
        : _sink(std::move(sink)), _bytes(bytes) {}
    future<> put(std::span<temporary_buffer<char>> data) override {
        for (auto& buf : data) {
            _bytes += buf.size();
        }
        return _sink.put(data);
    }
    future<> flush() override {
        return _sink.flush();
    }
    future<> close() override {
        return _sink.close();
    }
    temporary_buffer<char> allocate_buffer(size_t size) override {
        return _sink.allocate_buffer(size);
    }
    size_t buffer_size() const noexcept override {
        return _sink.buffer_size();
    }
    bool can_batch_flushes() const noexcept override {
        return _sink.can_batch_flushes();
    }
    void on_batch_flush_error() noexcept override {
        _sink.on_batch_flush_error();
    }
};
// end snippet: counting-streams

// snippet: server
// What a shard needs to serve its connections. Every shard has an
// instance; shard 0's also counts the clients of all the shards.
class server : public peering_sharded_service<server> {
    sharded<store>& _db;
    sharded<pubsub>& _ps;
    options _opts;
    semaphore _request_memory;      // this shard's requests' memory
    semaphore _reply_memory;        // this shard's waiting replies' memory
    gate _connections;              // held by this shard's connections
    shared_ptr<tls::server_credentials> _tls;   // if there's a TLS port
    size_t _clients = 0;            // on shard 0: clients of all shards
public:
    server(sharded<store>& db, sharded<pubsub>& ps, options opts)
        : _db(db), _ps(ps), _opts(opts)
        , _request_memory(opts.max_request_memory)
        , _reply_memory(opts.max_reply_memory) {}
    // Prepares this shard to accept connections: loads its TLS
    // credentials, and on shard 0, removes a Unix-domain socket left by
    // an earlier run.
    future<> prepare();
    // Accepts connections on this shard, until Shoal stops.
    future<> accept_connections();
    // Waits for this shard's connections to end.
    future<> wait_for_connections() { return _connections.close(); }
    future<> stop() {
        return _connections.is_closed() ? make_ready_future<>()
                                        : _connections.close();
    }
private:
    future<> accept_from(server_socket& listener);
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
                    // Execute them all, in order, and then wait for their
                    // replies, which wait for the log when they must.
                    std::vector<future<sstring>> replies;
                    replies.reserve(batch.size());
                    for (auto& w : batch) {
                        replies.push_back(execute_here(s, w.cmd, w.req));
                    }
                    return when_all_succeed(replies.begin(), replies.end());
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
    ordered_replies replies(out, _reply_memory);
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
            // While this shard's waiting replies use all its reply memory,
            // read no more requests.
            co_await replies.wait_for_memory();
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
                // (this shard's requests are executed at once, so they stay
                // in order; only a reply may wait, for the log), and if its
                // reply is ready, and no earlier reply waits, write it now,
                // as version 7 did. This keeps the speed of one shard.
                auto reply = execute_here(_db.local(), cmd, *req);
                if (reply.available() && !reply.failed()) {
                    auto r = reply.get();
                    if (!replies.write_now(r)) {
                        replies.add(make_ready_future<sstring>(std::move(r)),
                                    reader.take_units(),
                                    co_await replies.reserve());
                    }
                } else {
                    // It waits for the log: the next requests go on, and
                    // their replies wait behind it.
                    replies.add(std::move(reply), reader.take_units(),
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
                    co_await write_reply(out, std::move(*reply));
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
    // snippet: socket-options
    // Send each reply as soon as it's flushed (see chapter 4).
    s.set_nodelay(true);
    // Find out when a silent client has gone away, as Valkey does: when
    // the connection has been idle for tcp_keepalive, TCP sends a probe,
    // and then another every third of that, and when three probes go
    // unanswered, the connection fails.
    if (_opts.tcp_keepalive.count() > 0) {
        s.set_keepalive(true);
        s.set_keepalive_parameters(net::tcp_keepalive_params{
            .idle = _opts.tcp_keepalive,
            .interval = std::max(_opts.tcp_keepalive / 3,
                                 std::chrono::seconds(1)),
            .count = 3,
        });
    }
    // end snippet: socket-options
    // snippet: wrap-streams
    // The socket's streams, but with their source and sink wrapped in
    // ours, which count the bytes for INFO. The output stream batches its
    // flushes, as the socket's own does.
    input_stream<char> in(data_source(std::make_unique<counting_source>(
        s.input().detach(), db.net_input_bytes)));
    output_stream<char> out(
        data_sink(std::make_unique<counting_sink>(s.output().detach(),
                                                  db.net_output_bytes)),
        8192, output_stream_options{.batch_flushes = true});
    // end snippet: wrap-streams
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
    db.connected_clients++;
    auto forget_client = defer([&db] () noexcept { db.connected_clients--; });
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

// snippet: tls-credentials
future<> server::prepare() {
    if (this_shard_id() == 0 && !_opts.unix_socket.empty()) {
        // As Valkey does: a socket file can't be listened on again.
        co_await remove_file(_opts.unix_socket).handle_exception_type(
            [] (const std::system_error&) {});
    }
    if (!_opts.tls_port) {
        co_return;
    }
    tls::credentials_builder b;
    co_await b.set_x509_key_file(_opts.tls_cert_file, _opts.tls_key_file,
                                 tls::x509_crt_format::PEM);
    if (!_opts.tls_ca_cert_file.empty()) {
        co_await b.set_x509_trust_file(_opts.tls_ca_cert_file,
                                       tls::x509_crt_format::PEM);
    }
    b.set_client_auth(_opts.tls_auth_clients);
    if (this_shard_id() != 0) {
        _tls = b.build_server_credentials();
        co_return;
    }
    // Shard 0's credentials watch their files. When one changes, as when a
    // certificate is renewed, shard 0 reloads them, and rebuilds the other
    // shards' credentials from them, so new connections get the new
    // certificate without a restart.
    _tls = co_await b.build_reloadable_server_credentials(
            [this] (const tls::credentials_builder& b,
                    const std::unordered_set<sstring>& files,
                    std::exception_ptr e) -> future<> {
        // Once stopping has closed the gate, there is nothing to reload;
        // until then, holding it keeps the other shards' servers alive.
        if (_connections.is_closed()) {
            co_return;
        }
        auto holder = _connections.hold();
        if (e) {
            shoal_log.warn("could not reload {}: {}", fmt::join(files, ", "),
                           formattable(e));
            co_return;
        }
        co_await container().invoke_on_others([&b] (server& s) {
            b.rebuild(*s._tls);
        });
        shoal_log.info("reloaded {}", fmt::join(files, ", "));
    });
}
// end snippet: tls-credentials

// snippet: accept
future<> server::accept_connections() {
    auto& db = _db.local();
    // snippet: listen-options
    listen_options lo;
    lo.reuse_address = true;
    // Connections that Shoal is late to accept wait in a queue of this
    // length (which Linux caps at net.core.somaxconn).
    lo.listen_backlog = _opts.tcp_backlog;
    // end snippet: listen-options
    // A listener for each way to connect: TCP, TLS over TCP, and a
    // Unix-domain socket.
    std::vector<server_socket> listeners;
    for (auto& address : _opts.bind_addresses) {
        listeners.push_back(listen(socket_address(address, _opts.port), lo));
        if (_tls) {
            listeners.push_back(tls::listen(
                _tls, socket_address(address, _opts.tls_port), lo));
        }
    }
    if (!_opts.unix_socket.empty()) {
        listeners.push_back(listen(
            socket_address(unix_domain_addr(_opts.unix_socket)), lo));
    }
    // Stopping ends the wait for the next client, on every listener.
    auto stop_accepting = db.stopping.subscribe([&listeners] () noexcept {
        for (auto& l : listeners) {
            l.abort_accept();
        }
    });
    if (this_shard_id() == 0) {
        shoal_log.info("Shoal listening on port {} of {}, on {} shards",
                       _opts.port, fmt::join(_opts.bind_addresses, ", "),
                       this_smp_shard_count());
    }
    co_await coroutine::parallel_for_each(listeners,
            [this, &listeners] (server_socket& l) {
        // If one listener fails, stop the others, and let run() stop.
        return accept_from(l).handle_exception([&listeners] (auto e) {
            for (auto& l : listeners) {
                l.abort_accept();
            }
            return make_exception_future<>(std::move(e));
        });
    });
}

// Accepts connections from one listener, until Shoal stops.
future<> server::accept_from(server_socket& listener) {
    auto& db = _db.local();
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
    co_await srv.invoke_on_all(&server::prepare);
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

// snippet: resolve
// The addresses to listen on. Each of --bind's is an address, such as
// 127.0.0.1 or ::1, or a name, such as localhost, which is resolved into
// all its addresses, by /etc/hosts or by DNS.
future<std::vector<net::inet_address>> resolve(
        const std::vector<std::string>& names) {
    std::vector<net::inet_address> addresses;
    for (auto& name : names) {
        if (auto address = net::inet_address::parse_numerical(name)) {
            addresses.push_back(*address);
            continue;
        }
        auto host = co_await net::dns::get_host_by_name(name);
        for (auto& entry : host.addr_entries) {
            addresses.push_back(entry.addr);
        }
    }
    co_return addresses;
}
// end snippet: resolve

// snippet: shoal-main
future<> shoal_main(options opts) {
    sharded<store> db;
    sharded<pubsub> ps;
    sharded<server> srv;
    std::exception_ptr failure;
    try {
        // Resolve the names once, here, so that every shard listens on the
        // same addresses.
        opts.bind_addresses = co_await resolve(opts.bind);
        co_await db.start();
        // snippet: open-logs
        co_await recursive_touch_directory(opts.dir);
        co_await check_shard_count(opts.dir);
        auto start = std::chrono::steady_clock::now();
        co_await db.invoke_on_all([&opts] (store& s) {
            return s.open(opts.dir, opts.sync_always);
        });
        shoal_log.info("replayed the logs in {} in {:.3f} s", opts.dir,
                       std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - start).count());
        // end snippet: open-logs
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

// The value of --appendfsync: whether it's always.
bool appendfsync_option(const std::string& value) {
    if (value != "always" && value != "everysec") {
        throw bpo::error("--appendfsync must be always or everysec");
    }
    return value == "always";
}

// The value of --tls-auth-clients.
tls::client_auth client_auth_option(const std::string& option) {
    if (option == "yes") {
        return tls::client_auth::REQUIRE;
    } else if (option == "optional") {
        return tls::client_auth::REQUEST;
    } else if (option == "no") {
        return tls::client_auth::NONE;
    }
    throw bpo::error("--tls-auth-clients must be yes, no or optional");
}

int main(int argc, char** argv) {
    app_template::config cfg;
    cfg.name = "Shoal";
    cfg.description = "Shoal, a Redis-compatible key-value server";
    app_template app(std::move(cfg));
    app.add_options()
        ("bind", bpo::value<std::vector<std::string>>()->multitoken()
             ->default_value({"0.0.0.0"}, "0.0.0.0"),
         "the addresses, or names, to listen on (:: for every IPv6 and "
         "IPv4 address)")
        ("port", bpo::value<uint16_t>()->default_value(6379),
         "TCP port to listen on")
        ("tls-port", bpo::value<uint16_t>()->default_value(0),
         "TCP port to listen on for clients that use TLS (0: none)")
        ("tls-cert-file", bpo::value<std::string>()->default_value(""),
         "the server's certificate, in PEM format")
        ("tls-key-file", bpo::value<std::string>()->default_value(""),
         "the server's private key, in PEM format")
        ("tls-ca-cert-file", bpo::value<std::string>()->default_value(""),
         "the certificates that client certificates must be signed by")
        ("tls-auth-clients", bpo::value<std::string>()->default_value("yes"),
         "whether TLS clients must have a certificate: yes, no or optional")
        ("unixsocket", bpo::value<std::string>()->default_value(""),
         "Unix-domain socket to listen on (default: none)")
        ("tcp-backlog", bpo::value<int>()->default_value(511),
         "the most connections that wait to be accepted")
        ("tcp-keepalive", bpo::value<int>()->default_value(300),
         "close a silent client's connection if it doesn't answer TCP's "
         "probes, the first after this many seconds (0: never)")
        ("dir", bpo::value<std::string>()->default_value("shoal-data"),
         "the directory of the logs")
        ("appendfsync", bpo::value<std::string>()->default_value("everysec"),
         "when the log reaches the disk: always, before each change is "
         "acknowledged, or everysec, once a second")
        ("maxclients", bpo::value<size_t>()->default_value(10000),
         "refuse connections beyond this many clients")
        ("max-request-memory", bpo::value<size_t>()->default_value(0),
         "the most memory that the large arguments of the requests being "
         "read on each shard may use (default: a quarter of the shard's "
         "memory)")
        ("max-reply-memory", bpo::value<size_t>()->default_value(0),
         "the most memory that the replies waiting for their clients to "
         "read them may use on each shard, before Shoal stops reading "
         "requests (default: an eighth of the shard's memory)")
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
            .bind = config["bind"].as<std::vector<std::string>>(),
            .port = config["port"].as<uint16_t>(),
            .tls_port = config["tls-port"].as<uint16_t>(),
            .tls_cert_file = config["tls-cert-file"].as<std::string>(),
            .tls_key_file = config["tls-key-file"].as<std::string>(),
            .tls_ca_cert_file = config["tls-ca-cert-file"].as<std::string>(),
            .tls_auth_clients = client_auth_option(
                config["tls-auth-clients"].as<std::string>()),
            .unix_socket = config["unixsocket"].as<std::string>(),
            .tcp_backlog = config["tcp-backlog"].as<int>(),
            .tcp_keepalive = std::chrono::seconds(
                config["tcp-keepalive"].as<int>()),
            .dir = config["dir"].as<std::string>(),
            .sync_always = appendfsync_option(
                config["appendfsync"].as<std::string>()),
            .max_clients = config["maxclients"].as<size_t>(),
            .max_request_memory = memory_option(
                config["max-request-memory"].as<size_t>(), 4),
            .max_reply_memory = memory_option(
                config["max-reply-memory"].as<size_t>(), 8),
            .pubsub_max_pending = config["pubsub-max-pending"].as<size_t>(),
            .pubsub_max_total_pending = memory_option(
                config["pubsub-max-total-pending"].as<size_t>(), 4),
            .shutdown_grace = std::chrono::milliseconds(static_cast<int64_t>(
                config["shutdown-grace"].as<double>() * 1000)),
        });
    });
}
