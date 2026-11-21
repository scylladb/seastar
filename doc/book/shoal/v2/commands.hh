// Shoal's commands, and the data they work on.
#pragma once

#include "resp.hh"
#include <seastar/core/iostream.hh>
#include <unordered_map>

// snippet: store
// The data that Shoal stores: a map from keys to values.
using store = std::unordered_map<sstring, sstring>;

// Executes a request on the store, and writes its reply to out.
future<> execute(store& db, const request& req, output_stream<char>& out);
// end snippet: store
