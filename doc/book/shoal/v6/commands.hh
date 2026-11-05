// Shoal's commands, and the data they work on.
#pragma once

#include "resp.hh"
#include <seastar/core/abort_source.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/timer.hh>
#include <boost/intrusive/set.hpp>
#include <deque>
#include <list>
#include <memory>
#include <optional>
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

// snippet: entry
// A list's elements. A deque allocates them in small pieces, so a long
// list never needs one big allocation.
using list_value = std::deque<sstring>;

// A key's value, a string or a list, and, if the key has one, its
// expiration time.
struct entry {
    std::variant<sstring, list_value> value;
    std::unique_ptr<expiration> expires;
};
// end snippet: entry

// snippet: blocked-pop
// A client blocked in BLPOP. Whoever ends the wait first (a push to one of
// its keys, its timeout, or its connection closing) sets the promise, and
// takes the client off the lists of waiting clients of all its keys.
struct blocked_pop {
    // The key and the element it got, or nothing if it gave up waiting.
    promise<std::optional<std::pair<sstring, sstring>>> served;
    // Its place in the list of waiting clients of each of its keys.
    std::vector<std::pair<sstring, std::list<blocked_pop*>::iterator>> places;
    bool waiting = true;
};
// end snippet: blocked-pop

// snippet: store
// The data that Shoal stores, the keys' expiration times, ordered by time,
// with a timer armed for the earliest, and the clients blocked in BLPOP on
// each key, in the order in which they blocked.
struct store {
    std::unordered_map<sstring, entry> data;
    boost::intrusive::multiset<expiration> expirations;
    timer<expiry_clock> expiry_timer;
    std::unordered_map<sstring, std::list<blocked_pop*>> blocked;

    store();
    store(const store&) = delete;      // the timer's function points to it
};

// Executes a request on the store, and writes its reply to out. A command
// that waits, such as BLPOP, stops waiting when hung_up is triggered.
future<> execute(store& db, const request& req, output_stream<char>& out,
                 abort_source& hung_up);
// end snippet: store
