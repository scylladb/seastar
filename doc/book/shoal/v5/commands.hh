// Shoal's commands, and the data they work on.
#pragma once

#include "resp.hh"
#include <seastar/core/iostream.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/timer.hh>
#include <boost/intrusive/set.hpp>
#include <memory>
#include <unordered_map>

// snippet: store
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

// A key's value, and, if the key has one, its expiration time.
struct entry {
    sstring value;
    std::unique_ptr<expiration> expires;
};

// The data that Shoal stores, and the keys' expiration times, ordered by
// time, with a timer armed for the earliest.
struct store {
    std::unordered_map<sstring, entry> data;
    boost::intrusive::multiset<expiration> expirations;
    timer<expiry_clock> expiry_timer;

    store();
    store(const store&) = delete;      // the timer's function points to it
};

// Executes a request on the store, and writes its reply to out.
future<> execute(store& db, const request& req, output_stream<char>& out);
// end snippet: store
