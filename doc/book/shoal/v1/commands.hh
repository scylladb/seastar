// Shoal's commands, and the data they work on.
#pragma once

#include "resp.hh"
#include <unordered_map>

// snippet: store
// The data that Shoal stores: a map from keys to values.
using store = std::unordered_map<sstring, sstring>;

// Executes a request on the store, and returns the reply to send.
sstring execute(store& db, const request& req);
// end snippet: store
