// Replication (version 14): a replica copies the keys of a primary, and
// then applies its changes, which it fetches over Seastar's RPC.
#pragma once

#include "commands.hh"
#include <seastar/core/abort_source.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sharded.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/rpc/lz4_fragmented_compressor.hh>
#include <seastar/rpc/rpc.hh>
#include <concepts>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>

// snippet: serializer
// How the protocol's values are written into a message, and read back:
// integers in little-endian order, and a string as its length and its
// bytes. RPC finds these functions by their first parameter's type.
struct replication_serializer {};

template <typename Output, std::integral T>
void write(replication_serializer, Output& out, T v) {
    v = cpu_to_le(v);
    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

template <typename Input, std::integral T>
T read(replication_serializer, Input& in, rpc::type<T>) {
    T v;
    in.read(reinterpret_cast<char*>(&v), sizeof(v));
    return le_to_cpu(v);
}

template <typename Output>
void write(replication_serializer s, Output& out, const sstring& v) {
    write(s, out, uint32_t(v.size()));
    out.write(v.data(), v.size());
}

template <typename Input>
sstring read(replication_serializer s, Input& in, rpc::type<sstring>) {
    auto size = read(s, in, rpc::type<uint32_t>());
    auto v = uninitialized_string(size);
    in.read(v.data(), size);
    return v;
}
// end snippet: serializer

// snippet: verbs
// The verbs of the replication protocol. A replica calls them, for each
// of its shards, about the primary's shard of the same number:
//
//   SYNC(shard) -> (shards, feed, offset): starts a snapshot of the
//       shard's keys, which its feed receives from offset on, and returns
//       the primary's number of shards, and its feed's id.
//   FETCH(shard, feed, offset) -> changes: the feed's changes from offset
//       on, waiting for them if there are none yet.
enum class replication_verb : uint32_t {
    sync = 1,
    fetch = 2,
};

using replication_protocol =
    rpc::protocol<replication_serializer, replication_verb>;
// end snippet: verbs

// snippet: primary
// On every shard of a primary (--replication-port): the server of the
// replication protocol, whose connections a replica's shards open.
class primary {
    sharded<store>& _db;
    replication_protocol _proto{replication_serializer{}};
    rpc::lz4_fragmented_compressor::factory _lz4;
    // One server for each address that it listens on.
    std::deque<replication_protocol::server> _servers;
public:
    explicit primary(sharded<store>& db);
    void listen(socket_address address);
    future<> stop();
};
// end snippet: primary

// snippet: replica
// On every shard of a replica (--replicaof): a fiber that copies the keys
// of the primary's shard of the same number, then applies its changes.
class replica {
    store& _db;
    socket_address _primary;
    replication_protocol _proto{replication_serializer{}};
    rpc::lz4_fragmented_compressor::factory _lz4;
    std::unique_ptr<replication_protocol::client> _client;
    // The memory of the requests being parsed.
    semaphore _memory{std::numeric_limits<int32_t>::max()};
    abort_source _stopping;
    future<> _running = make_ready_future();
    friend class fetch_source;
public:
    replica(sharded<store>& db, socket_address primary);
    void start();
    future<> stop();
private:
    future<> run();
    future<> sync_and_apply();
    template <typename Call>
    auto call(Call call) -> decltype(call(*_client));
};
// end snippet: replica
