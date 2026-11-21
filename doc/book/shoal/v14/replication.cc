#include "replication.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

using namespace std::chrono_literals;

// The most changes that a FETCH returns at once.
constexpr size_t max_fetch = 128 * 1024;
// How long a FETCH waits for changes, when there are none, before it
// returns nothing.
constexpr auto max_wait = 1s;
// How long a replica waits for a reply, before it gives up on the call.
constexpr auto call_timeout = 5s;
// How long a replica waits before it calls again, after a call failed.
constexpr auto retry_pause = 1s;

// snippet: handlers
primary::primary(sharded<store>& db) : _db(db) {
    // Runs func on a shard of the primary's, for a replica that asked
    // about that shard.
    auto on_shard = [this] (uint32_t shard, auto func) {
        if (shard >= this_smp_shard_count()) {
            throw std::runtime_error(fmt::format("no shard {}", shard));
        }
        return _db.invoke_on(shard, std::move(func));
    };
    _proto.register_handler(replication_verb::sync, [on_shard] (
            uint32_t shard) {
        return on_shard(shard, [] (store& s) {
            auto snapshot = s.feed->start_snapshot();
            auto offset = s.feed->end();
            // The snapshot runs in the background, in the group of the
            // log's rewrite, the other work that looks at every key.
            (void)with_scheduling_group(s.groups.rewrite, [&s, snapshot] {
                return snapshot_to_feed(s, snapshot);
            });
            return rpc::tuple<uint32_t, uint64_t, uint64_t>(
                this_smp_shard_count(), s.feed->id, offset);
        });
    });
    _proto.register_handler(replication_verb::fetch, [on_shard] (
            rpc::opt_time_point timeout, uint32_t shard, uint64_t feed,
            uint64_t offset) {
        // Reply before the replica gives up on the call: with the changes
        // that came by then, which may be none.
        auto deadline = lowres_clock::now() + max_wait;
        if (timeout) {
            deadline = std::min(deadline, *timeout - 100ms);
        }
        return on_shard(shard, [=] (store& s) {
            if (s.feed->id != feed) {
                throw std::runtime_error(
                    "not the feed that the replica asked for");
            }
            return s.feed->fetch(offset, max_fetch, deadline);
        });
    });
}
// end snippet: handlers

// snippet: listen
void primary::listen(socket_address address) {
    rpc::server_options options;
    options.compressor_factory = &_lz4;
    _servers.emplace_back(_proto, options, address);
}

future<> primary::stop() {
    // A replica's FETCH may be waiting for this shard's changes: end the
    // wait, which the server would wait for.
    _db.local().feed->stop();
    for (auto& server : _servers) {
        co_await server.stop();
    }
}
// end snippet: listen

replica::replica(sharded<store>& db, socket_address primary)
    : _db(db.local()), _primary(primary) {}

void replica::start() {
    _db.replica = true;
    _running = run();
}

// snippet: replica-stop
future<> replica::stop() {
    _stopping.request_abort();
    // Fail the call in progress, if there is one. Nothing else may stop
    // the client from now on: an RPC client can only be stopped once.
    if (_client) {
        co_await _client->stop();
    }
    co_await std::move(_running);
}
// end snippet: replica-stop

// snippet: call
// Calls a verb. If the connection fails, or the call takes too long, which
// may mean that the primary is gone, makes a new connection, and calls
// again, after a pause, until Shoal stops.
template <typename Call>
auto replica::call(Call call) -> decltype(call(*_client)) {
    while (true) {
        _stopping.check();
        if (!_client) {
            rpc::client_options options;
            options.compressor_factory = &_lz4;
            _client = std::make_unique<replication_protocol::client>(
                _proto, options, _primary);
        }
        try {
            co_return co_await call(*_client);
        } catch (rpc::closed_error& e) {
            shoal_log.debug("replication: {}", e.what());
        } catch (rpc::timeout_error& e) {
            shoal_log.debug("replication: {}", e.what());
        }
        _stopping.check();      // stop() stopped the client
        auto failed = std::move(_client);
        co_await failed->stop();
        co_await sleep_abortable(retry_pause, _stopping);
    }
}
// end snippet: call

// snippet: fetch-source
// The changes of the primary's shard, from offset on, as a stream, from
// which Shoal's request reader reads them, as it reads a client's requests.
class fetch_source final : public data_source_impl {
    replica& _replica;
    uint64_t _feed;
    uint64_t _offset;
public:
    fetch_source(replica& r, uint64_t feed, uint64_t offset)
        : _replica(r), _feed(feed), _offset(offset) {}
    future<temporary_buffer<char>> get() override {
        auto fetch = _replica._proto.make_client<
            sstring (uint32_t, uint64_t, uint64_t)>(replication_verb::fetch);
        while (true) {
            auto changes = co_await _replica.call([&] (auto& client) {
                return fetch(client, call_timeout, this_shard_id(), _feed,
                             _offset);
            });
            // If none came in time, ask again.
            if (!changes.empty()) {
                _offset += changes.size();
                co_return std::move(changes).release();
            }
        }
    }
};
// end snippet: fetch-source

// snippet: sync-and-apply
// Copies the keys of the primary's shard, and applies its changes, until
// the primary can't go on, or Shoal stops.
future<> replica::sync_and_apply() {
    auto sync = _proto.make_client<
        rpc::tuple<uint32_t, uint64_t, uint64_t> (uint32_t)>(
            replication_verb::sync);
    auto [shards, feed, offset] = co_await call([&] (auto& client) {
        return sync(client, call_timeout, this_shard_id());
    });
    if (shards != this_smp_shard_count()) {
        throw std::runtime_error(fmt::format(
            "the primary has {} shards, and this replica {}", shards,
            this_smp_shard_count()));
    }
    // The snapshot's requests replace the keys they write, but the shard
    // may have others, which the primary no longer has.
    co_await apply_replicated(_db, request{"FLUSHALL"});
    input_stream<char> in(data_source(
        std::make_unique<fetch_source>(*this, feed, offset)));
    request_reader reader(in, _memory, _memory.available_units());
    std::exception_ptr error;
    try {
        while (auto req = co_await reader.read()) {
            co_await apply_replicated(_db, *req);
        }
    } catch (...) {
        error = std::current_exception();
    }
    co_await in.close();
    if (error) {
        std::rethrow_exception(error);
    }
}
// end snippet: sync-and-apply

// snippet: run
// Replicates the primary's shard until Shoal stops. When the primary
// can't go on, because it no longer has the changes that this replica
// needs next, or because it restarted, start again, with a new snapshot.
future<> replica::run() {
    while (!_stopping.abort_requested()) {
        try {
            co_await sync_and_apply();
        } catch (...) {
            if (!_stopping.abort_requested()) {
                shoal_log.warn("replication from {}: {}; starting again",
                               _primary, formattable(std::current_exception()));
            }
        }
        try {
            co_await sleep_abortable(retry_pause, _stopping);
        } catch (sleep_aborted&) {
        }
    }
}
// end snippet: run
