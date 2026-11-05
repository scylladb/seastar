// Shoal's commands, and the data they work on: each shard executes the
// commands on its own keys, and the connection's shard sends each command
// to the shards it needs.
#include "commands.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/core/smp.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/util/defer.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/core/timer.hh>
#include <fmt/format.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <chrono>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
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
void erase_key(store& db, std::unordered_map<sstring, entry>::iterator it) {
    persist_key(db, it->second);
    db.data.erase(it);
}

// Finds a key's value. A key that has expired is deleted first, so that,
// to every command, it doesn't exist.
auto lookup(store& db, const sstring& key) {
    auto it = db.data.find(key);
    if (it != db.data.end() && expired(it->second)) {
        erase_key(db, it);
        return db.data.end();
    }
    return it;
}
// end snippet: lookup

// snippet: expire-at
// Sets a key's expiration time, replacing any old one.
void expire_key_at(store& db, const sstring& key, entry& e,
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
        if ((option != "ex" && option != "px") || ttl || i + 1 == req.size()) {
            return error_reply("syntax error");
        }
        auto n = parse_integer(req[++i]);
        if (!n) {
            return error_reply("value is not an integer or out of range");
        }
        int64_t seconds = option == "ex" ? *n : *n / 1000;
        if (*n <= 0 || seconds > max_expire_seconds) {
            return error_reply("invalid expire time in 'set' command");
        }
        if (option == "ex") {
            ttl = std::chrono::seconds(*n);
        } else {
            ttl = std::chrono::milliseconds(*n);
        }
    }
    auto [it, inserted] = db.data.try_emplace(req[1]);
    it->second.value = req[2];
    if (ttl) {
        expire_key_at(db, it->first, it->second, expiry_clock::now() + *ttl);
    } else {
        // Like Redis, a SET without an expiration time removes any old one.
        persist_key(db, it->second);
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
            return f(s, req, keys);
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
// EXPIRE key seconds
sstring expire(store& db, const request& req) {
    auto seconds = parse_integer(req[2]);
    if (!seconds) {
        return error_reply("value is not an integer or out of range");
    }
    if (*seconds > max_expire_seconds) {
        return error_reply("invalid expire time in 'expire' command");
    }
    auto it = lookup(db, req[1]);
    if (it == db.data.end()) {
        return integer_reply(0);
    }
    if (*seconds <= 0) {
        // An expiration time that has already passed deletes the key.
        erase_key(db, it);
    } else {
        expire_key_at(db, it->first, it->second,
                      expiry_clock::now() + std::chrono::seconds(*seconds));
    }
    return integer_reply(1);
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

// snippet: keys
// The keys of this shard that match a pattern.
future<std::vector<sstring>> keys_here(store& db, const sstring& pattern) {
    std::deque<sstring> found;
    // Other clients' commands may change the table while we yield, so we
    // must not keep an iterator across a co_await. Instead, we look at one
    // bucket of the hash table at a time, and keep only the bucket's number.
    size_t buckets = db.data.bucket_count();
    size_t b = 0;
    while (b < buckets) {
        for (auto it = db.data.begin(b); it != db.data.end(b); ++it) {
            if (glob_match(pattern, it->first) && !expired(it->second)) {
                found.push_back(it->first);
            }
        }
        b++;
        co_await coroutine::maybe_yield();
        if (db.data.bucket_count() != buckets) {
            // The table grew, and its keys moved to new buckets: start again.
            co_await clear_gently(found);
            buckets = db.data.bucket_count();
            b = 0;
        }
    }
    // A future's value must move without throwing, which a deque can't.
    co_return std::vector<sstring>(std::make_move_iterator(found.begin()),
                                   std::make_move_iterator(found.end()));
}

// snippet: keys-reply
// KEYS pattern: every shard's keys.
future<std::optional<reply>> keys(sharded<store>& db, request req,
                                    abort_source&) {
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
// end snippet: keys-reply

// INFO [section]: a few of the statistics that Valkey's INFO returns,
// for all the shards together, whatever the section asked for.
future<std::optional<reply>> info(sharded<store>& db, request,
                                  abort_source&) {
    struct totals {
        uint64_t clients = 0, blocked = 0, input = 0, output = 0;
    };
    auto t = co_await db.map_reduce0([] (store& s) {
        return totals{s.connected_clients, s.blocked_clients,
                      s.net_input_bytes, s.net_output_bytes};
    }, totals{}, [] (totals a, totals b) {
        return totals{a.clients + b.clients, a.blocked + b.blocked,
                      a.input + b.input, a.output + b.output};
    });
    co_return bulk_string_reply(fmt::format(
        "# Clients\r\nconnected_clients:{}\r\nblocked_clients:{}\r\n"
        "\r\n# Stats\r\ntotal_net_input_bytes:{}\r\n"
        "total_net_output_bytes:{}\r\n",
        t.clients, t.blocked, t.input, t.output));
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
                        std::unordered_map<sstring, entry> data) {
    auto working = background.hold();
    while (!data.empty() && !background.is_closed()) {
        data.erase(data.begin());
        co_await coroutine::maybe_yield();
    }
}
// end snippet: destroy-gently

// snippet: flushall
// Deletes this shard's keys: moves them out of the store, which is quick,
// and destroys them gradually, now or in the background.
future<> flush_here(store& db, bool async) {
    // Forget all the expiration times, which is quick: the tree only needs
    // to forget its root, because the entries own the records.
    db.expirations.clear();
    db.expiry_timer.cancel();
    auto old = std::exchange(db.data, {});
    if (async && !db.background.is_closed()) {
        (void)destroy_gently(db.background, std::move(old));
    } else {
        // SYNC, or ASYNC while Shoal is stopping, which closed the gate.
        co_await clear_gently(old);
    }
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
// Shoal keeps nothing on disk, so there's nothing to save. As in Redis, the
// client gets no reply: Shoal closes its connection. Shard 0 stops Shoal.
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

// snippet: list-lookup
// Finds a key's list. Returns a null list if the key doesn't exist, and
// fails if it holds a string.
struct list_lookup {
    std::unordered_map<sstring, entry>::iterator it;
    list_value* list = nullptr;
    bool wrong_type = false;
};

list_lookup lookup_list(store& db, const sstring& key) {
    auto it = lookup(db, key);
    if (it == db.data.end()) {
        return {it};
    }
    auto* l = std::get_if<list_value>(&it->second.value);
    return {it, l, l == nullptr};
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
        auto& l = std::get<list_value>(it->second.value);
        auto element = std::move(l.front());
        l.pop_front();
        if (l.empty()) {
            erase_key(db, it);
        }
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
        l = &it->second.value.emplace<list_value>();
    }
    for (size_t i = 2; i < req.size(); i++) {
        if (front) {
            l->push_front(req[i]);
        } else {
            l->push_back(req[i]);
        }
    }
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
    auto element = std::move(front ? l->front() : l->back());
    if (front) {
        l->pop_front();
    } else {
        l->pop_back();
    }
    if (l->empty()) {
        erase_key(db, it);
    }
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
        auto element = std::move(l->front());
        l->pop_front();
        if (l->empty()) {
            erase_key(db, it);
        }
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
            return pop_or_wait(s, key, &w);
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

sstring execute_here(store& db, const command* cmd, const request& req) {
    if (!cmd) {
        return error_reply(fmt::format("unknown command '{}'", req[0]));
    }
    if (!right_arity(*cmd, req)) {
        return wrong_arguments(lowercase(req[0]));
    }
    return cmd->on_shard(db, req);
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

// snippet: store-constructor
store::store() : expiry_timer([this] { expire_keys(*this); }) {}

future<> store::stop() {
    expiry_timer.cancel();
    return background.close();
}
// end snippet: store-constructor
