// Shoal's commands, and the data they work on: on every shard, the keys
// that belong to it.
#pragma once

#include "resp.hh"
#include <seastar/core/abort_source.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/timer.hh>
#include <boost/intrusive/set.hpp>
#include <atomic>
#include <deque>
#include <list>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// The clock that measures when keys expire: cheap to read, and steady, so
// that changing the system's clock doesn't make keys expire early.
using expiry_clock = lowres_clock;

// When a key expires. It points back to its key, and it's linked, through
// the hook it inherits, into the store's tree of expiration times.
struct expiration : boost::intrusive::set_base_hook<
        boost::intrusive::link_mode<boost::intrusive::normal_link>> {
    expiry_clock::time_point when;
    const sstring* key;
    bool operator<(const expiration& other) const { return when < other.when; }
};

// A list's elements. A deque allocates them in small pieces, so a long
// list never needs one big allocation.
using list_value = std::deque<sstring>;

// A key's value, a string or a list, and, if the key has one, its
// expiration time.
struct entry {
    std::variant<sstring, list_value> value;
    std::unique_ptr<expiration> expires;
};

// snippet: blpop-wait
// A client blocked in BLPOP, on its connection's shard. It waits on every
// shard that owns one of its keys, and whoever ends the wait first sets
// done: a shard that pops an element for it, its timeout, or its client
// hanging up. Several shards may try at the same moment, so done is
// atomic: the one that changes it from false to true wins.
struct blpop_wait {
    std::atomic<bool> done = false;
    unsigned shard = this_shard_id();   // where the client is
    // Set on that shard: the key and the element, or nothing.
    promise<std::optional<std::pair<sstring, sstring>>> served;
};

// On a key's shard: a blpop_wait's place in the line of clients waiting
// for the key.
struct blocked_pop {
    blpop_wait* wait;
    sstring key;
    std::list<blocked_pop*>::iterator place;
};
// end snippet: blpop-wait

// snippet: store
// A shard's part of the data: the keys that belong to it, their expiration
// times, ordered by time, with a timer armed for the earliest, and the
// clients blocked in BLPOP on each key, in the order in which they
// blocked. Every shard has an instance.
class store : public peering_sharded_service<store> {
public:
    std::unordered_map<sstring, entry> data;
    boost::intrusive::multiset<expiration> expirations;
    timer<expiry_clock> expiry_timer;
    std::unordered_map<sstring, std::list<blocked_pop*>> blocked;
    // Every client blocked on this shard's keys, by its wait.
    std::unordered_multimap<blpop_wait*, std::unique_ptr<blocked_pop>> waiting;
    // Requested when Shoal should stop: on shard 0 by a signal or by
    // SHUTDOWN, and then by shard 0 on every shard.
    abort_source stopping;
    // Requested when Shoal stops waiting for the connections to end.
    abort_source giving_up;
    // Held by work that goes on in the background, such as FLUSHALL
    // ASYNC's, so that stopping can end it, and wait for it.
    gate background;
    // For INFO: this shard's clients, those of them waiting in BLPOP, and
    // the bytes read from them and written to them, which their streams
    // count (see main.cc).
    uint64_t connected_clients = 0;
    uint64_t blocked_clients = 0;
    uint64_t net_input_bytes = 0;
    uint64_t net_output_bytes = 0;

    store();
    store(const store&) = delete;      // the timer's function points to it
    // Called by sharded<store>::stop(): waits for the background work.
    future<> stop();
};
// end snippet: store

// snippet: owner
// The shard that a key belongs to. As in Redis Cluster, only the part of
// the key between the first '{' and the next '}', if there is one and it
// isn't empty, is hashed, so that keys such as {user1}.name and
// {user1}.email are on the same shard.
unsigned owner(std::string_view key);
// end snippet: owner

// snippet: execute-decl
// A command: its name, how many words it takes, where its keys are, and
// how it's executed. Its definition is in commands.cc.
struct command;

// The command of a request, or nothing if there is no such command.
const command* find_command(const request& req);

// The shard on which a request may be executed together with the
// connection's other requests to the same shard: the shard of all its
// keys, or this shard, for a command without keys, such as PING, or for
// one that doesn't exist, whose reply is an error. Nothing for any other
// request, which execute() must execute alone: a command that may wait
// or yield, such as BLPOP or KEYS, or one on keys of several shards.
std::optional<unsigned> batch_shard(const command* cmd, const request& req);

// Executes, on the shard that batch_shard() returned for it, a request
// that may be batched, and returns its reply.
sstring execute_here(store& db, const command* cmd, const request& req);

// Executes any request, on whichever shards it needs, and returns its
// reply, or nothing, if the client gets no reply. A command that waits,
// such as BLPOP, stops waiting when hung_up is triggered.
future<std::optional<reply>> execute(sharded<store>& db, request req,
                                       abort_source& hung_up);
// end snippet: execute-decl
