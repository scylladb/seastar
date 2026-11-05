// Shoal's commands, and the data they work on: on every shard, the keys
// that belong to it.
#pragma once

#include "log.hh"
#include "table.hh"
#include <seastar/core/scheduling.hh>
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
    const key_string* key;
    bool operator<(const expiration& other) const { return when < other.when; }
};

// snippet: list-value
// A list's elements. A deque allocates them in small pieces, so a long
// list never needs one big allocation. For --maxmemory (version 12), the
// list also counts the bytes of its elements that are too long for an
// sstring to keep inline, so that its footprint is known without looking
// at every element.
class list_value {
    std::deque<sstring> _elements;
    size_t _bytes = 0;
    static size_t outside(const sstring& s) {
        return s.size() > 15 ? s.size() : 0;
    }
public:
    void push_front(sstring s) {
        _bytes += outside(s);
        _elements.push_front(std::move(s));
    }
    void push_back(sstring s) {
        _bytes += outside(s);
        _elements.push_back(std::move(s));
    }
    sstring pop_front() {
        auto s = std::move(_elements.front());
        _elements.pop_front();
        _bytes -= outside(s);
        return s;
    }
    sstring pop_back() {
        auto s = std::move(_elements.back());
        _elements.pop_back();
        _bytes -= outside(s);
        return s;
    }
    size_t size() const { return _elements.size(); }
    bool empty() const { return _elements.empty(); }
    const sstring& operator[](size_t i) const { return _elements[i]; }
    auto begin() const { return _elements.begin(); }
    auto end() const { return _elements.end(); }
    size_t outside_bytes() const { return _bytes; }
};
// The list is kept through a pointer, so that the table's node stays
// small (see entry).
using list_ptr = std::unique_ptr<list_value>;
// end snippet: list-value

// snippet: entry
// A key's value, a string or a list, and, if the key has one, its
// expiration time. With its key, in the table's node, it takes 64 bytes,
// one cache line, so a command that looks up a key with a short string
// value, of up to 15 characters, needs no other.
struct entry {
    // For --maxmemory (version 12): about how much memory the key and its
    // value take, and when it was last used, in milliseconds.
    uint32_t footprint = 0;
    uint32_t last_used = 0;
    std::unique_ptr<expiration> expires;
    std::variant<sstring, list_ptr> value;
};
// end snippet: entry

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

// snippet: groups
// The scheduling groups of Shoal's own work, which it doesn't let delay
// its clients' requests: KEYS, which looks at every key, and the log's
// rewrite, both in groups with fewer shares than the default group.
struct scheduling_groups {
    scheduling_group keys;
    scheduling_group rewrite;
};
// end snippet: groups

// snippet: store
// A shard's part of the data: the keys that belong to it, their expiration
// times, ordered by time, with a timer armed for the earliest, and the
// clients blocked in BLPOP on each key, in the order in which they
// blocked. Every shard has an instance.
class store : public peering_sharded_service<store> {
public:
    key_table<entry> data;
    // snippet: store-memory
    // With --maxmemory: about how much memory the shard's keys and values
    // take, the most they may take, and how many keys were evicted to keep
    // them there.
    size_t used_memory = 0;
    size_t maxmemory = 0;
    uint64_t evicted_keys = 0;
    // end snippet: store-memory
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
    // snippet: store-log
    // The shard's append-only log, and whether every change waits until
    // the disk has it (--appendfsync always), or only the next commit,
    // once a second (everysec).
    std::unique_ptr<append_only_log> log;
    bool sync_always = false;
    // end snippet: store-log
    // snippet: store-rewrite
    // The groups that run KEYS and the rewrite of the log, the same on
    // every shard.
    scheduling_groups groups;
    // While the log is rewritten: the new log, which receives the
    // changes that the old one does, besides the rewrite's own requests.
    std::unique_ptr<append_only_log> rewriting;
    bool rewrite_in_progress = false;
    bool last_rewrite_failed = false;
    // end snippet: store-rewrite
    // For INFO: this shard's clients, those of them waiting in BLPOP, and
    // the bytes read from them and written to them, which their streams
    // count (see main.cc).
    uint64_t connected_clients = 0;
    uint64_t blocked_clients = 0;
    uint64_t net_input_bytes = 0;
    uint64_t net_output_bytes = 0;

    explicit store(scheduling_groups groups);
    store(const store&) = delete;      // the timer's function points to it
    // Opens the shard's log, in dir, and replays it.
    future<> open(sstring dir, bool always);
    // Rewrites the log, in the background, as BGREWRITEAOF asks: writes
    // the shard's data as a new log, as short as it can be, which then
    // replaces the log.
    future<> rewrite_log();
    // Whether a command, which began when the log had appends_before
    // requests, must wait for a commit: if it appended any, and either
    // every change waits, or too many wait to be written.
    bool must_commit(uint64_t appends_before) const;
    // Called by sharded<store>::stop(): stops the background work, waits
    // for it, and closes the log.
    future<> stop();
private:
    sstring _dir;
    future<> commit_periodically();
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
future<sstring> execute_here(store& db, const command* cmd,
                             const request& req);

// Executes any request, on whichever shards it needs, and returns its
// reply, or nothing, if the client gets no reply. A command that waits,
// such as BLPOP, stops waiting when hung_up is triggered.
future<std::optional<reply>> execute(sharded<store>& db, request req,
                                       abort_source& hung_up);
// end snippet: execute-decl
