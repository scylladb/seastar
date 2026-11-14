// Shoal's commands, and the data they work on.
#include "commands.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <fmt/format.h>
#include <algorithm>
#include <cctype>
#include <deque>
#include <optional>
#include <string>
#include <utility>

namespace {

sstring wrong_arguments(std::string_view command) {
    return error_reply(fmt::format(
        "wrong number of arguments for '{}' command", command));
}

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

// GET key
sstring get(store& db, const request& req) {
    auto it = db.find(req[1]);
    if (it == db.end()) {
        return null_reply();
    }
    return bulk_string_reply(it->second);
}

// SET key value
sstring set(store& db, const request& req) {
    db.insert_or_assign(req[1], req[2]);
    return simple_string_reply("OK");
}

// DEL key [key ...]
sstring del(store& db, const request& req) {
    int64_t deleted = 0;
    for (size_t i = 1; i < req.size(); i++) {
        deleted += db.erase(req[i]);
    }
    return integer_reply(deleted);
}

// EXISTS key [key ...]
sstring exists(store& db, const request& req) {
    int64_t found = 0;
    for (size_t i = 1; i < req.size(); i++) {
        found += db.contains(req[i]);
    }
    return integer_reply(found);
}

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
future<> keys(store& db, const request& req, output_stream<char>& out) {
    const sstring& pattern = req[1];
    std::deque<sstring> found;
    // Other clients' commands may change the table while we yield, so we
    // must not keep an iterator across a co_await. Instead, we look at one
    // bucket of the hash table at a time, and keep only the bucket's number.
    size_t buckets = db.bucket_count();
    size_t b = 0;
    while (b < buckets) {
        for (auto it = db.begin(b); it != db.end(b); ++it) {
            if (glob_match(pattern, it->first)) {
                found.push_back(it->first);
            }
        }
        b++;
        co_await coroutine::maybe_yield();
        if (db.bucket_count() != buckets) {
            // The table grew, and its keys moved to new buckets: start again.
            co_await clear_gently(found);
            buckets = db.bucket_count();
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

// snippet: flushall-async
// snippet: destroy-gently
// Destroys data gradually, in the background. The data is a parameter, so
// it lives in this coroutine's frame until the coroutine ends.
future<> destroy_gently(store data) {
    co_await clear_gently(data);
}
// end snippet: destroy-gently

// snippet: flushall
// FLUSHALL [ASYNC | SYNC]
future<> flushall(store& db, const request& req, output_stream<char>& out) {
    if (req.size() > 2 || (req.size() == 2 && lowercase(req[1]) != "async"
                                           && lowercase(req[1]) != "sync")) {
        co_return co_await out.write(error_reply("syntax error"));
    }
    // Move all the data out of the store, which is quick, and leaves the
    // store empty. Then destroy it gradually.
    store old = std::exchange(db, store());
    if (req.size() == 2 && lowercase(req[1]) == "async") {
        // Reply at once, and destroy the old data in the background.
        (void)destroy_gently(std::move(old));
    } else {
        co_await clear_gently(old);
    }
    co_await out.write(simple_string_reply("OK"));
}
// end snippet: flushall
// end snippet: flushall-async

// snippet: table
// A command: how many words its requests have, counting the command's own
// name (a negative number -n means at least n), and the function that
// executes it. A quick command returns its reply; a command that may take
// long writes its reply itself, and returns a future that becomes
// available when it's done.
struct command {
    int arity;
    sstring (*run)(store&, const request&) = nullptr;
    future<> (*run_long)(store&, const request&,
                         output_stream<char>&) = nullptr;
};

const std::unordered_map<std::string_view, command> commands = {
    {"ping", {.arity = -1, .run = ping}},
    {"echo", {.arity = 2, .run = echo}},
    {"get", {.arity = 2, .run = get}},
    {"set", {.arity = 3, .run = set}},
    {"del", {.arity = -2, .run = del}},
    {"exists", {.arity = -2, .run = exists}},
    {"keys", {.arity = 2, .run_long = keys}},
    {"flushall", {.arity = -1, .run_long = flushall}},
};
// end snippet: table

} // anonymous namespace

// snippet: execute
future<> execute(store& db, const request& req, output_stream<char>& out) {
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
    return cmd.run_long(db, req, out);
}
// end snippet: execute
