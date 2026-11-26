// Shoal's commands, and the data they work on.
#include "commands.hh"
#include <fmt/format.h>
#include <algorithm>
#include <cctype>
#include <string>

namespace {

// snippet: wrong-arguments
sstring wrong_arguments(std::string_view command) {
    return error_reply(fmt::format(
        "wrong number of arguments for '{}' command", command));
}
// end snippet: wrong-arguments

// snippet: commands
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
// end snippet: commands

// snippet: table
// A command: how many words its requests have, counting the command's own
// name (a negative number -n means at least n), and the function that
// executes it.
struct command {
    int arity;
    sstring (*run)(store&, const request&);
};

const std::unordered_map<std::string_view, command> commands = {
    {"ping", {-1, ping}},
    {"echo", {2, echo}},
    {"get", {2, get}},
    {"set", {3, set}},
    {"del", {-2, del}},
    {"exists", {-2, exists}},
};
// end snippet: table

} // anonymous namespace

// snippet: execute
sstring execute(store& db, const request& req) {
    std::string name(req[0]);
    std::ranges::transform(name, name.begin(), [] (unsigned char c) {
        return std::tolower(c);
    });
    auto it = commands.find(name);
    if (it == commands.end()) {
        return error_reply(fmt::format("unknown command '{}'", req[0]));
    }
    auto& cmd = it->second;
    int words = req.size();
    if ((cmd.arity > 0 && words != cmd.arity) ||
        (cmd.arity < 0 && words < -cmd.arity)) {
        return wrong_arguments(name);
    }
    return cmd.run(db, req);
}
// end snippet: execute
