// Shoal's commands, and the data they work on.
#include "commands.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/core/timer.hh>
#include <fmt/format.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>

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

// snippet: del-exists
// DEL key [key ...]
sstring del(store& db, const request& req) {
    int64_t deleted = 0;
    for (size_t i = 1; i < req.size(); i++) {
        auto it = lookup(db, req[i]);
        if (it != db.data.end()) {
            erase_key(db, it);
            deleted++;
        }
    }
    return integer_reply(deleted);
}

// EXISTS key [key ...]
sstring exists(store& db, const request& req) {
    int64_t found = 0;
    for (size_t i = 1; i < req.size(); i++) {
        found += lookup(db, req[i]) != db.data.end();
    }
    return integer_reply(found);
}
// end snippet: del-exists

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
// KEYS pattern
future<> keys(store& db, const request& req, output_stream<char>& out,
              abort_source&) {
    const sstring& pattern = req[1];
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
    co_await out.write(array_header_reply(found.size()));
    while (!found.empty()) {
        co_await out.write(bulk_string_reply(found.front()));
        found.pop_front();
    }
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
// FLUSHALL [ASYNC | SYNC]
future<> flushall(store& db, const request& req, output_stream<char>& out,
                  abort_source&) {
    if (req.size() > 2 || (req.size() == 2 && lowercase(req[1]) != "async"
                                           && lowercase(req[1]) != "sync")) {
        co_return co_await out.write(error_reply("syntax error"));
    }
    // Forget all the expiration times, which is quick: the tree only needs
    // to forget its root, because the entries own the records. Then move
    // all the data out of the store, which is quick too, and destroy it
    // gradually.
    db.expirations.clear();
    db.expiry_timer.cancel();
    auto old = std::exchange(db.data, {});
    if (req.size() == 2 && lowercase(req[1]) == "async"
                        && !db.background.is_closed()) {
        // Reply at once, and destroy the old data in the background.
        (void)destroy_gently(db.background, std::move(old));
    } else {
        // SYNC, or ASYNC while Shoal is stopping, which closed the gate.
        co_await clear_gently(old);
    }
    co_await out.write(simple_string_reply("OK"));
}
// end snippet: flushall

// snippet: shutdown
// SHUTDOWN [NOSAVE | SAVE]
// Shoal keeps nothing on disk, so there's nothing to save. As in Redis, the
// client gets no reply: Shoal closes its connection.
future<> shutdown(store& db, const request& req, output_stream<char>& out,
                  abort_source&) {
    if (req.size() > 2 || (req.size() == 2 && lowercase(req[1]) != "nosave"
                                           && lowercase(req[1]) != "save")) {
        co_return co_await out.write(error_reply("syntax error"));
    }
    db.stopping.request_abort();
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
// Ends a BLPOP's wait, with an element or without one, and takes the client
// off the lists of waiting clients of all its keys.
void end_wait(store& db, blocked_pop& b,
              std::optional<std::pair<sstring, sstring>> result) {
    b.waiting = false;
    for (auto& [key, place] : b.places) {
        auto it = db.blocked.find(key);
        it->second.erase(place);
        if (it->second.empty()) {
            db.blocked.erase(it);
        }
    }
    b.served.set_value(std::move(result));
}

// After a push to a list, gives its first elements to the clients blocked
// on its key, in the order in which they blocked, before any other command
// can take them. Deletes the key if that empties the list.
void serve_blocked(store& db, const sstring& key) {
    while (true) {
        auto waiters = db.blocked.find(key);
        auto it = db.data.find(key);
        if (waiters == db.blocked.end() || it == db.data.end()) {
            return;
        }
        auto& l = std::get<list_value>(it->second.value);
        auto element = std::move(l.front());
        l.pop_front();
        if (l.empty()) {
            erase_key(db, it);
        }
        end_wait(db, *waiters->second.front(), std::pair(key, std::move(element)));
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
future<> lrange(store& db, const request& req, output_stream<char>& out,
                abort_source&) {
    auto start = parse_integer(req[2]);
    auto stop = parse_integer(req[3]);
    if (!start || !stop) {
        co_return co_await out.write(
            error_reply("value is not an integer or out of range"));
    }
    auto [it, l, wrong_type] = lookup_list(db, req[1]);
    if (wrong_type) {
        co_return co_await out.write(wrong_type_reply());
    }
    int64_t size = l ? l->size() : 0;
    // Negative indexes count from the end; the range is cut to the list.
    int64_t first = std::max<int64_t>(*start < 0 ? size + *start : *start, 0);
    int64_t last = std::min<int64_t>(*stop < 0 ? size + *stop : *stop, size - 1);
    if (first > last) {
        co_return co_await out.write(array_header_reply(0));
    }
    // Copy the elements before writing them: while a write waits, other
    // commands may change the list.
    std::deque<sstring> elements(l->begin() + first, l->begin() + last + 1);
    co_await out.write(array_header_reply(elements.size()));
    while (!elements.empty()) {
        co_await out.write(bulk_string_reply(elements.front()));
        elements.pop_front();
    }
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
// BLPOP key [key ...] timeout
future<> blpop(store& db, const request& req, output_stream<char>& out,
               abort_source& hung_up) {
    auto timeout = parse_timeout(req.back());
    if (!timeout) {
        co_return co_await out.write(
            error_reply("timeout is not a float or out of range"));
    }
    if (*timeout < 0) {
        co_return co_await out.write(error_reply("timeout is negative"));
    }
    auto reply = [] (const sstring& key, const sstring& element) {
        return array_header_reply(2) + bulk_string_reply(key)
               + bulk_string_reply(element);
    };
    // If one of the keys has an element, pop it at once.
    for (size_t i = 1; i + 1 < req.size(); i++) {
        auto [it, l, wrong_type] = lookup_list(db, req[i]);
        if (wrong_type) {
            co_return co_await out.write(wrong_type_reply());
        }
        if (l) {
            // Pop the element before writing: the write may wait, and
            // while it does, other commands may change the list.
            auto element = std::move(l->front());
            l->pop_front();
            if (l->empty()) {
                erase_key(db, it);
            }
            co_return co_await out.write(reply(req[i], element));
        }
    }
    // Otherwise wait, at the end of each key's line of waiting clients,
    // until a push to one of them, the timeout, or the client hanging up.
    blocked_pop b;
    for (size_t i = 1; i + 1 < req.size(); i++) {
        auto& waiting = db.blocked[req[i]];
        b.places.emplace_back(req[i], waiting.insert(waiting.end(), &b));
    }
    auto give_up = [&db, &b] () noexcept {
        if (b.waiting) {
            end_wait(db, b, std::nullopt);
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
    auto result = co_await b.served.get_future();
    if (!result) {
        if (hung_up.abort_requested()) {
            // The client hung up, or Shoal is stopping: in both cases, the
            // connection is closing, and there is no one to reply to.
            co_return;
        }
        co_return co_await out.write(null_array_reply());
    }
    co_await out.write(reply(result->first, result->second));
}
// end snippet: blpop

// snippet: table
// A command: how many words its requests have, counting the command's own
// name (a negative number -n means at least n), and the function that
// executes it. A quick command returns its reply; a command that may take
// long writes its reply itself, and returns a future that becomes
// available when it's done.
struct command {
    int arity;
    sstring (*run)(store&, const request&) = nullptr;
    future<> (*run_long)(store&, const request&, output_stream<char>&,
                         abort_source&) = nullptr;
};

const std::unordered_map<std::string_view, command> commands = {
    {"ping", {.arity = -1, .run = ping}},
    {"echo", {.arity = 2, .run = echo}},
    {"get", {.arity = 2, .run = get}},
    {"set", {.arity = -3, .run = set}},
    {"del", {.arity = -2, .run = del}},
    {"exists", {.arity = -2, .run = exists}},
    {"expire", {.arity = 3, .run = expire}},
    {"ttl", {.arity = 2, .run = ttl}},
    {"persist", {.arity = 2, .run = persist}},
    {"lpush", {.arity = -3, .run = lpush}},
    {"rpush", {.arity = -3, .run = rpush}},
    {"lpop", {.arity = 2, .run = lpop}},
    {"rpop", {.arity = 2, .run = rpop}},
    {"llen", {.arity = 2, .run = llen}},
    {"lrange", {.arity = 4, .run_long = lrange}},
    {"blpop", {.arity = -3, .run_long = blpop}},
    {"keys", {.arity = 2, .run_long = keys}},
    {"flushall", {.arity = -1, .run_long = flushall}},
    {"shutdown", {.arity = -1, .run_long = shutdown}},
};
// end snippet: table

} // anonymous namespace

// snippet: execute
future<> execute(store& db, const request& req, output_stream<char>& out,
                 abort_source& hung_up) {
    auto name = lowercase(req[0]);
    auto it = commands.find(name);
    if (it == commands.end()) {
        auto error = fmt::format("unknown command '{}'", req[0]);
        return out.write(error_reply(error));
    }
    auto& cmd = it->second;
    int words = req.size();
    if ((cmd.arity > 0 && words != cmd.arity) ||
        (cmd.arity < 0 && words < -cmd.arity)) {
        return out.write(wrong_arguments(name));
    }
    if (cmd.run) {
        // write() copies the reply into the stream's buffer before it
        // returns, so the reply may be destroyed right away.
        return out.write(cmd.run(db, req));
    }
    return cmd.run_long(db, req, out, hung_up);
}
// end snippet: execute

// snippet: store-constructor
store::store() : expiry_timer([this] { expire_keys(*this); }) {}
// end snippet: store-constructor
