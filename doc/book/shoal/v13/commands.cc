// Shoal's commands, and the data they work on: each shard executes the
// commands on its own keys, and the connection's shard sends each command
// to the shards it needs.
#include "commands.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/core/seastar.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/coroutine/switch_to.hh>
#include <seastar/util/defer.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <fmt/format.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <chrono>
#include <deque>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>

// snippet: owner-def
unsigned owner(std::string_view key) {
    if (this_smp_shard_count() == 1) {
        return 0;               // no need to hash
    }
    auto open = key.find('{');
    if (open != key.npos) {
        auto close = key.find('}', open + 1);
        if (close != key.npos && close > open + 1) {
            key = key.substr(open + 1, close - open - 1);
        }
    }
    return std::hash<std::string_view>()(key) % this_smp_shard_count();
}
// end snippet: owner-def

namespace {

sstring wrong_arguments(std::string_view command) {
    return error_reply(fmt::format(
        "wrong number of arguments for '{}' command", command));
}

// snippet: parse-integer
// Parses a whole string as a decimal integer, or returns nothing.
std::optional<int64_t> parse_integer(std::string_view s) {
    int64_t n;
    auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (error != std::errc() || end != s.data() + s.size()) {
        return std::nullopt;
    }
    return n;
}

// The longest expiration time Shoal accepts, so that adding it to the
// clock's time can't overflow.
constexpr int64_t max_expire_seconds = 100LL * 365 * 24 * 3600;
// end snippet: parse-integer

// snippet: lookup
// Whether the key has an expiration time that has passed.
bool expired(const entry& e) {
    return e.expires && e.expires->when <= expiry_clock::now();
}

// Removes a key's expiration time, if it has one.
void persist_key(store& db, entry& e) {
    if (e.expires) {
        db.expirations.erase(db.expirations.iterator_to(*e.expires));
        e.expires.reset();
    }
}

// Deletes a key, and its expiration time.
void erase_key(store& db, key_table<entry>::iterator it) {
    persist_key(db, it->second);
    db.used_memory -= it->second.footprint;
    db.data.erase(it);
}

// snippet: footprint
// The time, in milliseconds, for remembering when a key was last used.
uint32_t lru_clock() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        expiry_clock::now().time_since_epoch()).count();
}

// About how much memory a key and its value take: the table's node, of 64
// bytes, and its share of the slots, 16 bytes each, in a small table
// between three eighths and three quarters full; what the key, a string
// value or a list's elements keep outside themselves; an expiration time;
// and a list's deque, which starts with a block of 512 bytes, for 32
// elements of 16 bytes.
size_t footprint(const key_string& key, const entry& e) {
    size_t bytes = 100 + (key.size() > key_inline_size ? key.size() : 0);
    if (e.expires) {
        bytes += sizeof(expiration);
    }
    if (auto* s = std::get_if<sstring>(&e.value)) {
        bytes += s->size() > 15 ? s->size() : 0;
    } else {
        auto& l = *std::get<list_ptr>(e.value);
        bytes += 672 + 16 * l.size() + l.outside_bytes();
    }
    return bytes;
}

// Called after a key may have changed: updates its footprint, and the
// shard's used_memory.
void account(store& db, std::string_view key) {
    auto it = db.data.find(key);
    if (it != db.data.end()) {
        auto& e = it->second;
        db.used_memory -= e.footprint;
        e.footprint = footprint(it->first, e);
        db.used_memory += e.footprint;
        e.last_used = lru_clock();
    }
}
// end snippet: footprint

// Finds a key's value. A key that has expired is deleted first, so that,
// to every command, it doesn't exist.
auto lookup(store& db, const sstring& key) {
    auto it = db.data.find(key);
    if (it != db.data.end()) {
        if (expired(it->second)) {
            erase_key(db, it);
            return db.data.end();
        }
        it->second.last_used = lru_clock();
    }
    return it;
}
// end snippet: lookup

// snippet: record
// Records a change in the shard's log: the request that, executed again
// when Shoal starts, makes the same change. While Shoal replays the log,
// the store has no log yet, so replaying records nothing.
template <typename Words>
void record(store& db, const Words& words) {
    if (db.log) {
        db.log->append(words);
    }
    if (db.rewriting) {
        db.rewriting->append(words);
    }
    // Every change is recorded, with its key first (but FLUSHALL, which
    // has none): count its memory.
    if (std::size(words) > 1) {
        account(db, *std::next(std::begin(words)));
    }
}

void record(store& db, std::initializer_list<std::string_view> words) {
    record<std::initializer_list<std::string_view>>(db, words);
}

// The time on the computer's clock, in milliseconds since 1970, which is
// how the log records expiration times, as Redis's does: a duration would
// start again when the log is replayed.
int64_t unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
// end snippet: record

// snippet: committed
// Returns value, once the requests appended to the log since it had
// appends_before are committed, if they must be.
template <typename T>
future<T> committed(store& db, uint64_t appends_before, T value) {
    if (!db.must_commit(appends_before)) {
        return make_ready_future<T>(std::move(value));
    }
    return db.log->commit().then([value = std::move(value)] () mutable {
        return std::move(value);
    });
}

// The number of requests appended to the log so far.
uint64_t appends(const store& db) {
    return db.log ? db.log->appends() : 0;
}
// end snippet: committed

// snippet: expire-at
// Sets a key's expiration time, replacing any old one.
void expire_key_at(store& db, const key_string& key, entry& e,
                   expiry_clock::time_point when) {
    if (e.expires) {
        db.expirations.erase(db.expirations.iterator_to(*e.expires));
    } else {
        e.expires = std::make_unique<expiration>();
        e.expires->key = &key;
    }
    e.expires->when = when;
    db.expirations.insert(*e.expires);
    // If this key now expires first, the timer must fire earlier.
    if (&*db.expirations.begin() == e.expires.get()) {
        db.expiry_timer.rearm(when);
    }
}
// end snippet: expire-at

// snippet: expire-keys
// How many keys the timer's function deletes before it lets other work run.
constexpr int expire_batch = 1000;

// The expiry timer's function: deletes the keys whose expiration time has
// passed, and arms the timer again for the next one. A timer's function
// can't yield, so it deletes only a batch of keys, and if more are due,
// arms the timer to run again right after other work.
void expire_keys(store& db) {
    auto now = expiry_clock::now();
    for (int i = 0; i < expire_batch; i++) {
        if (db.expirations.empty()) {
            return;
        }
        auto& first = *db.expirations.begin();
        if (first.when > now) {
            db.expiry_timer.rearm(first.when);
            return;
        }
        erase_key(db, db.data.find(*first.key));
    }
    // A whole batch expired, and more keys may be due: expire the next
    // batch as soon as the reactor has let other work run.
    db.expiry_timer.rearm(now);
}
// end snippet: expire-keys

std::string lowercase(std::string_view s) {
    std::string ret(s);
    std::ranges::transform(ret, ret.begin(), [] (unsigned char c) {
        return std::tolower(c);
    });
    return ret;
}


// PING [message]
sstring ping(store&, const request& req) {
    if (req.size() == 1) {
        return simple_string_reply("PONG");
    }
    if (req.size() == 2) {
        return bulk_string_reply(req[1]);
    }
    return wrong_arguments("ping");
}

// ECHO message
sstring echo(store&, const request& req) {
    return bulk_string_reply(req[1]);
}

// snippet: get
// GET key
sstring get(store& db, const request& req) {
    auto it = lookup(db, req[1]);
    if (it == db.data.end()) {
        return null_reply();
    }
    auto* value = std::get_if<sstring>(&it->second.value);
    if (!value) {
        return wrong_type_reply();
    }
    return bulk_string_reply(*value);
}
// end snippet: get

// snippet: set
// SET key value [EX seconds | PX milliseconds]
sstring set(store& db, const request& req) {
    std::optional<expiry_clock::duration> ttl;
    for (size_t i = 3; i < req.size(); i++) {
        auto option = lowercase(req[i]);
        if ((option != "ex" && option != "px" && option != "pxat")
                || ttl || i + 1 == req.size()) {
            return error_reply("syntax error");
        }
        auto n = parse_integer(req[++i]);
        if (!n) {
            return error_reply("value is not an integer or out of range");
        }
        // PXAT, a time in milliseconds since 1970, is what the log records.
        if (*n <= 0 || (option == "ex" && *n > max_expire_seconds)) {
            return error_reply("invalid expire time in 'set' command");
        }
        int64_t ms = option == "ex" ? *n * 1000
                   : option == "px" ? *n : *n - unix_ms();
        if (ms / 1000 > max_expire_seconds) {
            return error_reply("invalid expire time in 'set' command");
        }
        ttl = std::chrono::milliseconds(ms);
    }
    auto [it, inserted] = db.data.try_emplace(req[1]);
    it->second.value = req[2];
    if (ttl) {
        expire_key_at(db, it->first, it->second, expiry_clock::now() + *ttl);
        auto at = std::chrono::duration_cast<std::chrono::milliseconds>(*ttl)
                  .count() + unix_ms();
        record(db, {"SET", req[1], req[2], "PXAT", fmt::to_string(at)});
    } else {
        // Like Redis, a SET without an expiration time removes any old one.
        persist_key(db, it->second);
        record(db, {"SET", req[1], req[2]});
    }
    return simple_string_reply("OK");
}
// end snippet: set

// snippet: by-shard
// The positions in req of the keys that belong to each shard: the keys are
// req[first], and every step-th word after it.
std::vector<std::vector<size_t>> keys_by_shard(const request& req,
                                               size_t first, size_t step) {
    std::vector<std::vector<size_t>> by_shard(this_smp_shard_count());
    for (size_t i = first; i < req.size(); i += step) {
        by_shard[owner(req[i])].push_back(i);
    }
    return by_shard;
}

// The positions in req of all its keys, req[first] and every step-th word
// after it, for a command whose keys all belong to the shard it runs on.
std::vector<size_t> all_keys(const request& req, size_t first, size_t step) {
    std::vector<size_t> keys;
    for (size_t i = first; i < req.size(); i += step) {
        keys.push_back(i);
    }
    return keys;
}

// Runs f on every shard that owns some of req's keys, with the positions of
// its keys, and adds up the results.
future<int64_t> count_on_owners(
        sharded<store>& db, const request& req,
        const std::vector<std::vector<size_t>>& by_shard,
        int64_t (*f)(store&, const request&, const std::vector<size_t>&)) {
    return map_reduce(this_smp().all_shards(),
                      [&db, &req, &by_shard, f] (unsigned shard) {
        if (by_shard[shard].empty()) {
            return make_ready_future<int64_t>(0);
        }
        return db.invoke_on(shard, [&req, &keys = by_shard[shard], f]
                                   (store& s) {
            auto before = appends(s);
            return committed(s, before, f(s, req, keys));
        });
    }, int64_t(0), std::plus<int64_t>());
}
// end snippet: by-shard

// snippet: del-exists
// DEL key [key ...]
int64_t del_here(store& db, const request& req,
                 const std::vector<size_t>& keys) {
    int64_t deleted = 0;
    for (size_t i : keys) {
        auto it = lookup(db, req[i]);
        if (it != db.data.end()) {
            erase_key(db, it);
            record(db, {"DEL", req[i]});
            deleted++;
        }
    }
    return deleted;
}

sstring del_on_shard(store& db, const request& req) {
    return integer_reply(del_here(db, req, all_keys(req, 1, 1)));
}

future<std::optional<reply>> del(sharded<store>& db, request req,
                                   abort_source&) {
    auto by_shard = keys_by_shard(req, 1, 1);
    co_return integer_reply(
        co_await count_on_owners(db, req, by_shard, del_here));
}

// EXISTS key [key ...]
int64_t exists_here(store& db, const request& req,
                    const std::vector<size_t>& keys) {
    int64_t found = 0;
    for (size_t i : keys) {
        found += lookup(db, req[i]) != db.data.end();
    }
    return found;
}

sstring exists_on_shard(store& db, const request& req) {
    return integer_reply(exists_here(db, req, all_keys(req, 1, 1)));
}

future<std::optional<reply>> exists(sharded<store>& db, request req,
                                      abort_source&) {
    auto by_shard = keys_by_shard(req, 1, 1);
    co_return integer_reply(
        co_await count_on_owners(db, req, by_shard, exists_here));
}
// end snippet: del-exists

// snippet: mget-mset
// MGET key [key ...]: the values, in the order of the keys, or nil for a
// key that doesn't exist or doesn't hold a string.
std::vector<std::optional<sstring>> mget_here(store& db, const request& req,
                                              const std::vector<size_t>& keys) {
    std::vector<std::optional<sstring>> found;
    for (size_t i : keys) {
        auto it = lookup(db, req[i]);
        auto* value = it == db.data.end()
            ? nullptr : std::get_if<sstring>(&it->second.value);
        found.push_back(value ? std::optional(*value) : std::nullopt);
    }
    return found;
}

sstring mget_reply(const std::vector<std::optional<sstring>>& values) {
    std::string reply(array_header_reply(values.size()));
    for (auto& value : values) {
        reply += value ? bulk_string_reply(*value) : null_reply();
    }
    return sstring(reply);
}

sstring mget_on_shard(store& db, const request& req) {
    return mget_reply(mget_here(db, req, all_keys(req, 1, 1)));
}

future<std::optional<reply>> mget(sharded<store>& db, request req,
                                    abort_source&) {
    auto by_shard = keys_by_shard(req, 1, 1);
    std::vector<std::optional<sstring>> values(req.size() - 1);
    co_await coroutine::parallel_for_each(this_smp().all_shards(),
            [&] (unsigned shard) -> future<> {
        if (by_shard[shard].empty()) {
            co_return;
        }
        // Each shard returns its keys' values, which this shard puts in
        // their places.
        auto found = co_await db.invoke_on(shard,
                [&req, &keys = by_shard[shard]] (store& s) {
            return mget_here(s, req, keys);
        });
        for (size_t k = 0; k < found.size(); k++) {
            values[by_shard[shard][k] - 1] = std::move(found[k]);
        }
    });
    co_return mget_reply(values);
}

// MSET key value [key value ...]. Each shard sets its keys at once, but the
// shards do it independently: another client may see some of the new
// values before the others, which Redis's MSET never shows.
int64_t mset_here(store& db, const request& req,
                  const std::vector<size_t>& keys) {
    for (size_t i : keys) {
        auto [it, inserted] = db.data.try_emplace(req[i]);
        it->second.value = req[i + 1];
        persist_key(db, it->second);
        record(db, {"SET", req[i], req[i + 1]});
    }
    return 0;
}

sstring mset_on_shard(store& db, const request& req) {
    if (req.size() % 2 == 0) {
        return wrong_arguments("mset");
    }
    mset_here(db, req, all_keys(req, 1, 2));
    return simple_string_reply("OK");
}

future<std::optional<reply>> mset(sharded<store>& db, request req,
                                    abort_source&) {
    if (req.size() % 2 == 0) {
        co_return wrong_arguments("mset");
    }
    auto by_shard = keys_by_shard(req, 1, 2);
    co_await count_on_owners(db, req, by_shard, mset_here);
    co_return simple_string_reply("OK");
}
// end snippet: mget-mset

// snippet: expire
// Sets a key's expiration time, ms milliseconds from now, and records it
// in the log as a time on the computer's clock.
sstring expire_in(store& db, const request& req, int64_t ms) {
    auto it = lookup(db, req[1]);
    if (it == db.data.end()) {
        return integer_reply(0);
    }
    if (ms <= 0) {
        // An expiration time that has already passed deletes the key.
        erase_key(db, it);
        record(db, {"DEL", req[1]});
    } else {
        expire_key_at(db, it->first, it->second,
                      expiry_clock::now() + std::chrono::milliseconds(ms));
        record(db, {"PEXPIREAT", req[1], fmt::to_string(unix_ms() + ms)});
    }
    return integer_reply(1);
}

// EXPIRE key seconds
sstring expire(store& db, const request& req) {
    auto seconds = parse_integer(req[2]);
    if (!seconds) {
        return error_reply("value is not an integer or out of range");
    }
    if (*seconds > max_expire_seconds) {
        return error_reply("invalid expire time in 'expire' command");
    }
    return expire_in(db, req, std::max<int64_t>(*seconds, 0) * 1000);
}

// PEXPIREAT key milliseconds-since-1970, which the log records.
sstring pexpireat(store& db, const request& req) {
    auto at = parse_integer(req[2]);
    if (!at) {
        return error_reply("value is not an integer or out of range");
    }
    auto ms = std::max<int64_t>(*at, 0) - unix_ms();
    if (ms / 1000 > max_expire_seconds) {
        return error_reply("invalid expire time in 'pexpireat' command");
    }
    return expire_in(db, req, ms);
}

// TTL key
sstring ttl(store& db, const request& req) {
    auto it = lookup(db, req[1]);
    if (it == db.data.end()) {
        return integer_reply(-2);
    }
    if (!it->second.expires) {
        return integer_reply(-1);
    }
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        it->second.expires->when - expiry_clock::now());
    // Round to the nearest second, as Redis does.
    return integer_reply((left.count() + 500) / 1000);
}

// PERSIST key
sstring persist(store& db, const request& req) {
    auto it = lookup(db, req[1]);
    if (it == db.data.end() || !it->second.expires) {
        return integer_reply(0);
    }
    persist_key(db, it->second);
    record(db, req);
    return integer_reply(1);
}
// end snippet: expire

// Matches the character c against the pattern element at p[i], which is
// not '*', and sets next to the index of the element after it.
bool match_one(std::string_view p, size_t i, char c, size_t& next) {
    if (p[i] == '?') {
        next = i + 1;
        return true;
    }
    if (p[i] == '\\' && i + 1 < p.size()) {
        next = i + 2;
        return p[i + 1] == c;
    }
    if (p[i] != '[') {
        next = i + 1;
        return p[i] == c;
    }
    // A set of characters, such as [abc], [a-z] or [^0-9].
    i++;
    bool negate = i < p.size() && p[i] == '^';
    if (negate) {
        i++;
    }
    bool matched = false;
    while (i < p.size() && p[i] != ']') {
        if (p[i] == '\\' && i + 1 < p.size()) {
            matched |= p[i + 1] == c;
            i += 2;
        } else if (i + 2 < p.size() && p[i + 1] == '-') {
            auto [low, high] = std::minmax(p[i], p[i + 2]);
            matched |= low <= c && c <= high;
            i += 3;
        } else {
            matched |= p[i] == c;
            i++;
        }
    }
    next = i < p.size() ? i + 1 : i;
    return matched != negate;
}

// snippet: glob-match
// Checks if s matches a glob-style pattern, in which '*' matches any
// string, '?' any character, and [...] any character of a set.
bool glob_match(std::string_view pattern, std::string_view s) {
    size_t p = 0, i = 0;
    // Where to continue if what follows the last '*' doesn't match.
    std::optional<size_t> star_p;
    size_t star_i = 0;
    while (i < s.size()) {
        size_t next;
        if (p < pattern.size() && pattern[p] == '*') {
            star_p = ++p;
            star_i = i;
        } else if (p < pattern.size() && match_one(pattern, p, s[i], next)) {
            p = next;
            i++;
        } else if (star_p) {
            // Let the last '*' match one more character, and try again.
            p = *star_p;
            i = ++star_i;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        p++;
    }
    return p == pattern.size();
}
// end snippet: glob-match

// snippet: clear-gently
// Destroys a container's elements one at a time, yielding whenever the
// task quota is used up, instead of destroying them all at once.
template <typename Container>
future<> clear_gently(Container& c) {
    while (!c.empty()) {
        c.erase(c.begin());
        co_await coroutine::maybe_yield();
    }
}
// end snippet: clear-gently

// The same for a store's table, a hundred keys at a time.
template <typename Value>
future<> clear_gently(key_table<Value>& table) {
    while (!table.empty()) {
        table.erase_some(100);
        co_await coroutine::maybe_yield();
    }
}

// snippet: keys
// The keys of this shard that match a pattern.
future<std::vector<sstring>> keys_here(store& db, const sstring& pattern) {
    std::deque<sstring> found;
    // Other clients' commands may change the table while we yield, so we
    // must not keep an iterator across a co_await. Instead, we look at one
    // small table at a time, and keep only where the next one begins,
    // which stays right however the table changes.
    size_t position = 0;
    do {
        position = db.data.scan(position, [&] (auto& element) {
            auto& [key, e] = element;
            if (glob_match(pattern, key) && !expired(e)) {
                found.emplace_back(key.data(), key.size());
            }
        });
        co_await coroutine::maybe_yield();
    } while (position != 0);
    // A future's value must move without throwing, which a deque can't.
    co_return std::vector<sstring>(std::make_move_iterator(found.begin()),
                                   std::make_move_iterator(found.end()));
}

// snippet: keys-group
// KEYS pattern: every shard's keys.
future<std::optional<reply>> keys(sharded<store>& db, request req,
                                    abort_source&) {
    // KEYS looks at every key, on every shard, and builds a reply that may
    // be huge: all of it runs in the keys group, with fewer shares than
    // the requests of other clients, on this shard and, since a message
    // carries its sender's group, on the others too.
    co_await coroutine::switch_to(db.local().groups.keys);
    // end snippet: keys-group
    auto found = co_await db.map([&pattern = req[1]] (store& s) {
        return keys_here(s, pattern);
    });
    size_t n = 0;
    for (auto& keys : found) {
        n += keys.size();
    }
    // The reply holds every key in the database, so build it in pieces,
    // and let other work run while it's built.
    reply_builder reply;
    reply.append(array_header_reply(n));
    for (auto& keys : found) {
        for (auto& key : keys) {
            reply.append_bulk_string(key);
            co_await coroutine::maybe_yield();
        }
    }
    co_return std::move(reply).finish();
}

// INFO [section]: a few of the statistics that Valkey's INFO returns,
// for all the shards together, whatever the section asked for.
future<std::optional<reply>> info(sharded<store>& db, request,
                                  abort_source&) {
    struct totals {
        uint64_t clients = 0, blocked = 0, input = 0, output = 0;
        bool rewriting = false, rewrite_failed = false;
        uint64_t memory = 0, maxmemory = 0, evicted = 0;
    };
    auto t = co_await db.map_reduce0([] (store& s) {
        return totals{s.connected_clients, s.blocked_clients,
                      s.net_input_bytes, s.net_output_bytes,
                      s.rewrite_in_progress, s.last_rewrite_failed,
                      s.used_memory, s.maxmemory, s.evicted_keys};
    }, totals{}, [] (totals a, totals b) {
        return totals{a.clients + b.clients, a.blocked + b.blocked,
                      a.input + b.input, a.output + b.output,
                      a.rewriting || b.rewriting,
                      a.rewrite_failed || b.rewrite_failed,
                      a.memory + b.memory, a.maxmemory + b.maxmemory,
                      a.evicted + b.evicted};
    });
    co_return bulk_string_reply(fmt::format(
        "# Clients\r\nconnected_clients:{}\r\nblocked_clients:{}\r\n"
        "\r\n# Memory\r\nused_memory:{}\r\nmaxmemory:{}\r\n"
        "\r\n# Persistence\r\naof_rewrite_in_progress:{}\r\n"
        "aof_last_bgrewrite_status:{}\r\n"
        "\r\n# Stats\r\ntotal_net_input_bytes:{}\r\n"
        "total_net_output_bytes:{}\r\nevicted_keys:{}\r\n",
        t.clients, t.blocked, t.memory, t.maxmemory, int(t.rewriting),
        t.rewrite_failed ? "err" : "ok", t.input, t.output, t.evicted));
}

// DBSIZE: the number of keys, on all the shards together.
future<std::optional<reply>> dbsize(sharded<store>& db, request,
                                      abort_source&) {
    co_return integer_reply(co_await db.map_reduce0(
        [] (store& s) { return s.data.size(); },
        size_t(0), std::plus<size_t>()));
}
// end snippet: keys

// snippet: destroy-gently
// Destroys data gradually, in the background. The data is a parameter, so
// it lives in this coroutine's frame until the coroutine ends. It holds the
// background gate while it works, and when the gate is closed, which Shoal
// does when it stops, it stops too: the rest of the data is destroyed at
// once, with the frame.
future<> destroy_gently(gate& background,
                        key_table<entry> data) {
    auto working = background.hold();
    while (!data.empty() && !background.is_closed()) {
        data.erase_some(100);
        co_await coroutine::maybe_yield();
    }
}
// end snippet: destroy-gently

// snippet: flushall
// Deletes this shard's keys: moves them out of the store, which is quick,
// and destroys them gradually, now or in the background.
future<> flush_here(store& db, bool async) {
    auto before = appends(db);
    record(db, {"FLUSHALL"});
    // Forget all the expiration times, which is quick: the tree only needs
    // to forget its root, because the entries own the records.
    db.expirations.clear();
    db.expiry_timer.cancel();
    auto old = std::exchange(db.data, {});
    db.used_memory = 0;
    if (async && !db.background.is_closed()) {
        (void)destroy_gently(db.background, std::move(old));
    } else {
        // SYNC, or ASYNC while Shoal is stopping, which closed the gate.
        co_await clear_gently(old);
    }
    co_await committed(db, before, 0);
}

// FLUSHALL [ASYNC | SYNC]: on every shard.
future<std::optional<reply>> flushall(sharded<store>& db, request req,
                                        abort_source&) {
    if (req.size() > 2 || (req.size() == 2 && lowercase(req[1]) != "async"
                                           && lowercase(req[1]) != "sync")) {
        co_return error_reply("syntax error");
    }
    bool async = req.size() == 2 && lowercase(req[1]) == "async";
    co_await db.invoke_on_all([async] (store& s) {
        return flush_here(s, async);
    });
    co_return simple_string_reply("OK");
}
// end snippet: flushall

// snippet: shutdown
// SHUTDOWN [NOSAVE | SAVE]
// Either way, Shoal commits its logs as it stops, as Redis does with its
// append-only file. As in Redis, the client gets no reply: Shoal closes
// its connection. Shard 0 stops Shoal.
future<std::optional<reply>> shutdown(sharded<store>& db, request req,
                                        abort_source&) {
    if (req.size() > 2 || (req.size() == 2 && lowercase(req[1]) != "nosave"
                                           && lowercase(req[1]) != "save")) {
        co_return error_reply("syntax error");
    }
    co_await db.invoke_on(0, [] (store& s) { s.stopping.request_abort(); });
    co_return std::nullopt;
}
// end snippet: shutdown

// snippet: bgrewriteaof
// BGREWRITEAOF: rewrites every shard's log, in the background, in the
// rewrite group. As in Redis, the reply comes at once, and INFO tells
// when the rewrite has finished.
future<std::optional<reply>> bgrewriteaof(sharded<store>& db, request,
                                            abort_source&) {
    if (!co_await start_rewrite(db)) {
        co_return error_reply(
            "Background append only file rewriting already in progress");
    }
    co_return simple_string_reply(
        "Background append only file rewriting started");
}
// end snippet: bgrewriteaof

// snippet: list-lookup
// Finds a key's list. Returns a null list if the key doesn't exist, and
// fails if it holds a string.
struct list_lookup {
    key_table<entry>::iterator it;
    list_value* list = nullptr;
    bool wrong_type = false;
};

list_lookup lookup_list(store& db, const sstring& key) {
    auto it = lookup(db, key);
    if (it == db.data.end()) {
        return {it};
    }
    auto* l = std::get_if<list_ptr>(&it->second.value);
    return {it, l ? l->get() : nullptr, l == nullptr};
}
// end snippet: list-lookup

// snippet: serve-blocked
// Takes a client off the line of clients waiting for its key.
void forget_blocked(store& db, blocked_pop* b) {
    auto line = db.blocked.find(b->key);
    line->second.erase(b->place);
    if (line->second.empty()) {
        db.blocked.erase(line);
    }
    auto [first, last] = db.waiting.equal_range(b->wait);
    for (auto it = first; it != last; ++it) {
        if (it->second.get() == b) {
            db.waiting.erase(it);
            return;
        }
    }
}

// Takes a BLPOP off the lines of all its keys on this shard.
void forget_wait(store& db, blpop_wait* w) {
    while (true) {
        auto it = db.waiting.find(w);
        if (it == db.waiting.end()) {
            return;
        }
        forget_blocked(db, it->second.get());
    }
}

// After a push to a list, gives its first elements to the clients blocked
// on its key, in the order in which they blocked, before any other command
// can take them. A client whose wait already ended, on another shard or by
// its timeout, is skipped. Deletes the key if that empties the list.
void serve_blocked(store& db, const sstring& key) {
    while (true) {
        auto line = db.blocked.find(key);
        auto it = db.data.find(key);
        if (line == db.blocked.end() || it == db.data.end()) {
            return;
        }
        blocked_pop* b = line->second.front();
        blpop_wait* w = b->wait;
        sstring k = b->key;
        forget_blocked(db, b);
        if (w->done.exchange(true)) {
            continue;
        }
        auto& l = *std::get<list_ptr>(it->second.value);
        auto element = l.pop_front();
        if (l.empty()) {
            erase_key(db, it);
        }
        record(db, {"LPOP", key});
        // Give the element to the client's shard, which waits for it. The
        // message can't fail, so nothing needs to wait for its future.
        (void)smp::submit_to(w->shard,
                [w, k = std::move(k), element = std::move(element)] () mutable {
            w->served.set_value(std::pair(std::move(k), std::move(element)));
        });
    }
}
// end snippet: serve-blocked

// snippet: push
// LPUSH key element [element ...] and RPUSH key element [element ...]
sstring push(store& db, const request& req, bool front) {
    auto [it, l, wrong_type] = lookup_list(db, req[1]);
    if (wrong_type) {
        return wrong_type_reply();
    }
    if (!l) {
        it = db.data.try_emplace(req[1]).first;
        l = it->second.value.emplace<list_ptr>(
                std::make_unique<list_value>()).get();
    }
    for (size_t i = 2; i < req.size(); i++) {
        if (front) {
            l->push_front(req[i]);
        } else {
            l->push_back(req[i]);
        }
    }
    record(db, req);
    // Reply with the list's length after the push, before serving the
    // clients blocked on it, as Redis does.
    auto reply = integer_reply(l->size());
    serve_blocked(db, req[1]);
    return reply;
}

sstring lpush(store& db, const request& req) { return push(db, req, true); }
sstring rpush(store& db, const request& req) { return push(db, req, false); }

// LPOP key and RPOP key
sstring pop(store& db, const request& req, bool front) {
    auto [it, l, wrong_type] = lookup_list(db, req[1]);
    if (wrong_type) {
        return wrong_type_reply();
    }
    if (!l) {
        return null_reply();
    }
    auto element = front ? l->pop_front() : l->pop_back();
    if (l->empty()) {
        erase_key(db, it);
    }
    record(db, req);
    return bulk_string_reply(element);
}

sstring lpop(store& db, const request& req) { return pop(db, req, true); }
sstring rpop(store& db, const request& req) { return pop(db, req, false); }
// end snippet: push

// LLEN key
sstring llen(store& db, const request& req) {
    auto [it, l, wrong_type] = lookup_list(db, req[1]);
    if (wrong_type) {
        return wrong_type_reply();
    }
    return integer_reply(l ? l->size() : 0);
}

// LRANGE key start stop
sstring lrange(store& db, const request& req) {
    auto start = parse_integer(req[2]);
    auto stop = parse_integer(req[3]);
    if (!start || !stop) {
        return error_reply("value is not an integer or out of range");
    }
    auto [it, l, wrong_type] = lookup_list(db, req[1]);
    if (wrong_type) {
        return wrong_type_reply();
    }
    int64_t size = l ? l->size() : 0;
    // Negative indexes count from the end; the range is cut to the list.
    int64_t first = std::max<int64_t>(*start < 0 ? size + *start : *start, 0);
    int64_t last = std::min<int64_t>(*stop < 0 ? size + *stop : *stop, size - 1);
    if (first > last) {
        return array_header_reply(0);
    }
    std::string reply(array_header_reply(last - first + 1));
    for (int64_t i = first; i <= last; i++) {
        reply += bulk_string_reply((*l)[i]);
    }
    return sstring(reply);
}

// snippet: parse-timeout
// Parses BLPOP's timeout, in seconds, which may have a fraction.
std::optional<double> parse_timeout(std::string_view s) {
    double seconds;
    auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), seconds);
    if (error != std::errc() || end != s.data() + s.size() ||
            !std::isfinite(seconds) || seconds > max_expire_seconds) {
        return std::nullopt;
    }
    return seconds;
}
// end snippet: parse-timeout

// snippet: blpop
// On a key's shard: what a BLPOP found there.
enum class pop_outcome { popped, waiting, wrong_type, over };

// Pops the first element of the key's list for a BLPOP, if the list has
// one and the BLPOP's wait isn't over, or else puts the BLPOP at the end
// of the key's line of waiting clients.
std::pair<pop_outcome, sstring> pop_or_wait(store& db, const sstring& key,
                                            blpop_wait* w) {
    auto [it, l, wrong_type] = lookup_list(db, key);
    if (wrong_type) {
        return {pop_outcome::wrong_type, {}};
    }
    if (l) {
        if (w->done.exchange(true)) {
            return {pop_outcome::over, {}};
        }
        auto element = l->pop_front();
        if (l->empty()) {
            erase_key(db, it);
        }
        record(db, {"LPOP", key});
        return {pop_outcome::popped, std::move(element)};
    }
    if (w->done.load()) {
        return {pop_outcome::over, {}};
    }
    auto b = std::make_unique<blocked_pop>(blocked_pop{w, key, {}});
    auto& line = db.blocked[key];
    b->place = line.insert(line.end(), b.get());
    db.waiting.emplace(w, std::move(b));
    return {pop_outcome::waiting, {}};
}

// BLPOP key [key ...] timeout, on the client's shard.
future<std::optional<reply>> blpop(sharded<store>& db, request req,
                                     abort_source& hung_up) {
    auto timeout = parse_timeout(req.back());
    if (!timeout) {
        co_return error_reply("timeout is not a float or out of range");
    }
    if (*timeout < 0) {
        co_return error_reply("timeout is negative");
    }
    blpop_wait w;
    // The timeout and the client hanging up end the wait, unless a shard
    // has already popped an element for it.
    auto give_up = [&w] () noexcept {
        if (!w.done.exchange(true)) {
            w.served.set_value(std::nullopt);
        }
    };
    timer<> expiry(give_up);
    if (*timeout > 0) {
        expiry.arm(std::chrono::duration_cast<timer<>::duration>(
            std::chrono::duration<double>(*timeout)));
    }
    auto subscription = hung_up.subscribe(give_up);
    if (!subscription) {
        give_up();      // the client had already hung up
    }
    // Visit the keys in order: pop an element from the first one that has
    // one, or wait in the line of each.
    std::vector<unsigned> shards;
    std::optional<sstring> error;
    for (size_t i = 1; i + 1 < req.size() && !w.done.load(); i++) {
        shards.push_back(owner(req[i]));
        auto [outcome, element] = co_await db.invoke_on(shards.back(),
                [&key = req[i], &w] (store& s) {
            auto before = appends(s);
            return committed(s, before, pop_or_wait(s, key, &w));
        });
        if (outcome == pop_outcome::popped) {
            w.served.set_value(std::pair(req[i], std::move(element)));
        } else if (outcome == pop_outcome::wrong_type) {
            if (!w.done.exchange(true)) {
                w.served.set_value(std::nullopt);
                error = wrong_type_reply();
            }
        }
    }
    // While the client waits, INFO counts it as blocked.
    auto& local = db.local();
    local.blocked_clients++;
    auto unblocked = defer([&local] () noexcept { local.blocked_clients--; });
    auto result = co_await w.served.get_future();
    // No shard may use w after it's destroyed, with this coroutine's frame:
    // take it off the lines of all its keys.
    co_await parallel_for_each(shards, [&db, &w] (unsigned shard) {
        return db.invoke_on(shard, [&w] (store& s) { forget_wait(s, &w); });
    });
    if (error) {
        co_return error;
    }
    if (!result) {
        if (hung_up.abort_requested()) {
            // The client hung up, or Shoal is stopping: in both cases, the
            // connection is closing, and there is no one to reply to.
            co_return std::nullopt;
        }
        co_return null_array_reply();
    }
    co_return array_header_reply(2) + bulk_string_reply(result->first)
              + bulk_string_reply(result->second);
}
// end snippet: blpop

// snippet: table
// A command: how many words its requests have, counting the command's own
// name (a negative number -n means at least n), where its keys are, and
// how it's executed. Its keys are req[first_key] (none if first_key is 0),
// and every key_step-th word after it (only one if key_step is 0). When
// they all belong to one shard, which is always so for a command on one
// key, on_shard executes it there, and returns its reply; such requests
// may be batched. Otherwise, run executes it here, on the connection's
// shard, sending work to other shards as it needs.
} // anonymous namespace

struct command {
    int arity;
    size_t first_key = 0;
    size_t key_step = 0;
    sstring (*on_shard)(store&, const request&) = nullptr;
    future<std::optional<reply>> (*run)(sharded<store>&, request,
                                          abort_source&) = nullptr;
};

namespace {

const std::unordered_map<std::string_view, command> commands = {
    {"ping", {.arity = -1, .on_shard = ping}},
    {"echo", {.arity = 2, .on_shard = echo}},
    {"get", {.arity = 2, .first_key = 1, .on_shard = get}},
    {"set", {.arity = -3, .first_key = 1, .on_shard = set}},
    {"del", {.arity = -2, .first_key = 1, .key_step = 1,
             .on_shard = del_on_shard, .run = del}},
    {"exists", {.arity = -2, .first_key = 1, .key_step = 1,
                .on_shard = exists_on_shard, .run = exists}},
    {"mget", {.arity = -2, .first_key = 1, .key_step = 1,
              .on_shard = mget_on_shard, .run = mget}},
    {"mset", {.arity = -3, .first_key = 1, .key_step = 2,
              .on_shard = mset_on_shard, .run = mset}},
    {"expire", {.arity = 3, .first_key = 1, .on_shard = expire}},
    {"pexpireat", {.arity = 3, .first_key = 1, .on_shard = pexpireat}},
    {"ttl", {.arity = 2, .first_key = 1, .on_shard = ttl}},
    {"persist", {.arity = 2, .first_key = 1, .on_shard = persist}},
    {"lpush", {.arity = -3, .first_key = 1, .on_shard = lpush}},
    {"rpush", {.arity = -3, .first_key = 1, .on_shard = rpush}},
    {"lpop", {.arity = 2, .first_key = 1, .on_shard = lpop}},
    {"rpop", {.arity = 2, .first_key = 1, .on_shard = rpop}},
    {"llen", {.arity = 2, .first_key = 1, .on_shard = llen}},
    {"lrange", {.arity = 4, .first_key = 1, .on_shard = lrange}},
    {"dbsize", {.arity = 1, .run = dbsize}},
    {"info", {.arity = -1, .run = info}},
    {"blpop", {.arity = -3, .run = blpop}},
    {"keys", {.arity = 2, .run = keys}},
    {"flushall", {.arity = -1, .run = flushall}},
    {"bgrewriteaof", {.arity = 1, .run = bgrewriteaof}},
    {"shutdown", {.arity = -1, .run = shutdown}},
};
// end snippet: table

} // anonymous namespace

const command* find_command(const request& req) {
    // Lowercase the name into a small buffer, rather than a new string:
    // this runs for every request. No command's name is longer.
    char name[16];
    if (req[0].size() > sizeof(name)) {
        return nullptr;
    }
    std::ranges::transform(req[0], name, [] (unsigned char c) {
        return std::tolower(c);
    });
    auto it = commands.find(std::string_view(name, req[0].size()));
    return it == commands.end() ? nullptr : &it->second;
}

// Whether a request has the right number of words for its command.
static bool right_arity(const command& cmd, const request& req) {
    int words = req.size();
    return cmd.arity > 0 ? words == cmd.arity : words >= -cmd.arity;
}

// snippet: batch
std::optional<unsigned> batch_shard(const command* cmd, const request& req) {
    if (!cmd || !right_arity(*cmd, req)) {
        return this_shard_id();     // the reply is an error, made here
    }
    if (!cmd->on_shard) {
        return std::nullopt;
    }
    if (!cmd->first_key) {
        return this_shard_id();     // a command without keys
    }
    unsigned shard = owner(req[cmd->first_key]);
    if (cmd->key_step) {
        for (size_t i = cmd->first_key + cmd->key_step; i < req.size();
             i += cmd->key_step) {
            if (owner(req[i]) != shard) {
                return std::nullopt;
            }
        }
    }
    return shard;
}

// snippet: evict
// With --maxmemory: while the shard's keys take more than their part, evict
// keys, as Redis's allkeys-lru policy does: sample a few keys at random,
// and evict the one that was used least recently. At most 64 keys at a time,
// so that one command never waits long.
void evict(store& db) {
    static thread_local std::minstd_rand random;
    for (int i = 0; i < 64 && db.used_memory > db.maxmemory; i++) {
        auto victim = db.data.end();
        for (int sample = 0; sample < 5; sample++) {
            auto it = db.data.sample(random);
            if (it == db.data.end()) {
                return;
            }
            if (victim == db.data.end() ||
                    it->second.last_used < victim->second.last_used) {
                victim = it;
            }
        }
        // As in Redis, the log records an eviction as a DEL.
        sstring key(victim->first.data(), victim->first.size());
        erase_key(db, victim);
        record(db, {"DEL", key});
        db.evicted_keys++;
    }
}
// end snippet: evict

future<sstring> execute_here(store& db, const command* cmd,
                             const request& req) {
    if (db.maxmemory && db.used_memory > db.maxmemory) {
        evict(db);
    }
    if (!cmd) {
        return make_ready_future<sstring>(
            error_reply(fmt::format("unknown command '{}'", req[0])));
    }
    if (!right_arity(*cmd, req)) {
        return make_ready_future<sstring>(wrong_arguments(lowercase(req[0])));
    }
    auto before = appends(db);
    // If the commit fails, as it would on a failing disk, the client gets
    // an error: the change was made, in memory, but may be lost.
    return committed(db, before, cmd->on_shard(db, req)).handle_exception(
            [] (std::exception_ptr e) {
        return error_reply(fmt::format("can't write the log: {}",
                                       formattable(e)));
    });
}
// end snippet: batch

// snippet: execute
future<std::optional<reply>> execute(sharded<store>& db, request req,
                                       abort_source& hung_up) {
    auto* cmd = find_command(req);
    if (auto shard = batch_shard(cmd, req)) {
        // Executed on that shard, which reads the request from this
        // shard's memory: req lives in this coroutine's frame, unchanged,
        // until the reply comes back.
        co_return co_await db.invoke_on(*shard, [cmd, &req] (store& s) {
            return execute_here(s, cmd, req);
        });
    }
    co_return co_await cmd->run(db, std::move(req), hung_up);
}
// end snippet: execute

// snippet: replay
// Executes a request from the log, as Shoal starts. The log holds only
// requests that Shoal itself recorded; one that failed then, such as an
// LPUSH to a key that held a string, fails again, and changes nothing.
void replay(store& db, const request& req) {
    if (lowercase(req[0]) == "flushall") {
        db.expirations.clear();
        db.data.clear();
        db.used_memory = 0;
        return;
    }
    execute_here(db, find_command(req), req).ignore_ready_future();
}
// end snippet: replay

// snippet: store-constructor
store::store(scheduling_groups groups)
    : expiry_timer([this] { expire_keys(*this); }), groups(groups) {}

// snippet: store-open
future<> store::open(sstring dir, bool always) {
    sync_always = always;
    _dir = dir;
    auto path = fmt::format("{}/shard-{}.aof", dir, this_shard_id());
    // A rewrite that Shoal didn't finish, because it crashed, or stopped,
    // leaves a new log that never replaced the old one: remove it.
    if (co_await file_exists(path + ".rewrite")) {
        co_await remove_file(path + ".rewrite");
    }
    log = co_await append_only_log::open(
        path, [this] (const request& req) { replay(*this, req); });
    if (!sync_always) {
        (void)commit_periodically();
    }
}

// The bytes that may wait in memory to be committed, before the requests
// that append more must wait for a commit.
constexpr uint64_t max_uncommitted = 1 << 20;

bool store::must_commit(uint64_t appends_before) const {
    return log && log->appends() != appends_before &&
           (sync_always || log->uncommitted() >= max_uncommitted);
}

// With --appendfsync everysec: commits once a second, until Shoal stops.
future<> store::commit_periodically() {
    auto holder = background.hold();
    while (true) {
        try {
            co_await sleep_abortable(std::chrono::seconds(1), stopping);
        } catch (sleep_aborted&) {
            co_return;
        }
        try {
            co_await log->commit();
        } catch (...) {
            shoal_log.error("can't commit the log: {}",
                            formattable(std::current_exception()));
        }
    }
}

future<> store::stop() {
    expiry_timer.cancel();
    // Stopping ends the periodic commits (which, if Shoal failed to start,
    // weren't told to stop), and the log is committed and closed last.
    stopping.request_abort();
    co_await background.close();
    if (log) {
        co_await log->close();
    }
}
// end snippet: store-open
// end snippet: store-constructor

// snippet: write-key
// Writes a key into a new log, as the requests that recreate it: SET, or
// DEL and RPUSH, which replace whatever the key held, so that the changes
// made to it earlier in the rewrite, which the new log already has, are
// replaced too; and then PEXPIREAT, if the key expires.
void write_key(append_only_log& to, std::string_view key, const entry& e) {
    if (auto* s = std::get_if<sstring>(&e.value)) {
        to.append(std::initializer_list<std::string_view>{"SET", key, *s});
    } else {
        to.append(std::initializer_list<std::string_view>{"DEL", key});
        // A thousand elements to a request, so that a long list doesn't
        // become one huge request.
        std::vector<std::string_view> words{"RPUSH", key};
        for (auto& element : *std::get<list_ptr>(e.value)) {
            words.push_back(element);
            if (words.size() == 1002) {
                to.append(words);
                words.resize(2);
            }
        }
        if (words.size() > 2) {
            to.append(words);
        }
    }
    if (e.expires) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            e.expires->when - expiry_clock::now()).count();
        to.append(std::initializer_list<std::string_view>{
            "PEXPIREAT", key, fmt::to_string(unix_ms() + left)});
    }
}
// end snippet: write-key

// snippet: rewrite
future<> store::rewrite_log() {
    // Started in the background, by BGREWRITEAOF, which set
    // rewrite_in_progress; if Shoal is stopping, don't start.
    auto holder = background.try_hold();
    if (!holder || !log) {      // stopping, or no log (--appendonly no)
        rewrite_in_progress = false;
        co_return;
    }
    auto path = fmt::format("{}/shard-{}.aof", _dir, this_shard_id());
    auto tmp = path + ".rewrite";
    std::exception_ptr error;
    try {
        rewriting = co_await append_only_log::create(tmp);
        // From here on, every change reaches the new log too. Look at one
        // small table at a time, as KEYS does, since other requests change
        // the table while we yield.
        size_t position = 0;
        do {
            position = data.scan(position, [this] (auto& element) {
                if (!expired(element.second)) {
                    write_key(*rewriting, element.first, element.second);
                }
            });
            if (rewriting->uncommitted() >= max_uncommitted) {
                // Written, but not flushed: the new log must be durable
                // only when it replaces the old one, and hand_over()
                // commits it then.
                co_await rewriting->write();
            } else {
                co_await coroutine::maybe_yield();
            }
        } while (position != 0 && !stopping.abort_requested());
        if (!stopping.abort_requested()) {
            // Clients' commits wait for the hand-over, so it mustn't run
            // with the rewrite's few shares: it runs in their group.
            co_await coroutine::switch_to(default_scheduling_group());
            co_await log->hand_over(*rewriting, tmp, path);
            auto old = std::exchange(log, std::move(rewriting));
            // The old log's waiting commits now commit the new one.
            co_await old->close();
        }
    } catch (...) {
        error = std::current_exception();
    }
    if (rewriting) {
        // Stopped, or failed: keep the old log, and drop the new one.
        auto dropped = std::move(rewriting);
        try {
            co_await dropped->close();
            co_await remove_file(tmp);
        } catch (...) {
            error = error ? error : std::current_exception();
        }
    }
    if (error) {
        shoal_log.error("can't rewrite {}: {}", path, formattable(error));
    }
    last_rewrite_failed = bool(error);
    rewrite_in_progress = false;
}
// end snippet: rewrite

// snippet: admin-functions
// Starts rewriting every shard's log, in the background, in the rewrite
// group, unless a rewrite is in progress on some shard: then it returns
// false. For BGREWRITEAOF, and the admin API (version 13).
future<bool> start_rewrite(sharded<store>& db) {
    auto busy = co_await db.map_reduce0(
        [] (store& s) { return s.rewrite_in_progress; }, false,
        std::logical_or<bool>());
    if (busy) {
        co_return false;
    }
    co_await db.invoke_on_all([] (store& s) {
        // Marked at once, so that the next request sees it, though the
        // rewrite starts only when the rewrite group gets to run.
        if (!s.rewrite_in_progress) {
            s.rewrite_in_progress = true;
            // Not awaited: rewrite_log() holds the store's background gate.
            (void)with_scheduling_group(s.groups.rewrite, [&s] {
                return s.rewrite_log();
            });
        }
    });
    co_return true;
}

// Deletes a key, by executing DEL, which also logs the deletion and waits
// as the log requires; returns whether the key existed.
future<bool> delete_key(sharded<store>& db, sstring key) {
    abort_source never;
    auto r = co_await execute(db, request{"DEL", std::move(key)}, never);
    co_return r && std::get<sstring>(*r) == integer_reply(1);
}

// The keys that match a pattern, on every shard, as KEYS finds them, and
// in the keys group, as KEYS runs.
future<std::vector<sstring>> matching_keys(sharded<store>& db,
                                           sstring pattern) {
    co_await coroutine::switch_to(db.local().groups.keys);
    auto found = co_await db.map([&pattern] (store& s) {
        return keys_here(s, pattern);
    });
    std::vector<sstring> all;
    for (auto& keys : found) {
        for (auto& key : keys) {
            all.push_back(std::move(key));
            co_await coroutine::maybe_yield();
        }
    }
    co_return all;
}
// end snippet: admin-functions
