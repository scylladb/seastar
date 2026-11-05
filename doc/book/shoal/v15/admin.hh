// Shoal's admin API: an HTTP server, on every shard, whose routes report on
// the store and act on it (version 13).
#pragma once
#include "commands.hh"
#include <seastar/http/routes.hh>

using namespace seastar;

// Adds the admin API's routes to a shard's routes.
void set_admin_routes(httpd::routes& r, sharded<store>& db);
