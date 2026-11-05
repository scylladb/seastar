// Shoal's admin API (version 13). The routes and the JSON objects come from
// api/admin.json, from which seastar-json2code.py generates admin.json.hh.
#include "admin.hh"
#include "admin.json.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/http/exception.hh>
#include <seastar/json/json_elements.hh>
#include <memory>
#include <vector>

namespace aj = httpd::admin_json;

namespace {

// snippet: get-info
// GET /v1/info: the keys and the memory of all the shards together.
future<json::json_return_type> get_info(sharded<store>& db) {
    struct totals {
        long keys = 0, used_memory = 0, maxmemory = 0, evicted_keys = 0;
    };
    auto t = co_await db.map_reduce0([] (store& s) {
        return totals{long(s.data.size()), long(s.used_memory),
                      long(s.maxmemory), long(s.evicted_keys)};
    }, totals{}, [] (totals a, totals b) {
        return totals{a.keys + b.keys, a.used_memory + b.used_memory,
                      a.maxmemory + b.maxmemory,
                      a.evicted_keys + b.evicted_keys};
    });
    aj::info info;
    info.shards = this_smp_shard_count();
    info.keys = t.keys;
    info.used_memory = t.used_memory;
    info.maxmemory = t.maxmemory;
    info.evicted_keys = t.evicted_keys;
    co_return info;
}
// end snippet: get-info

// GET /v1/shards: the keys and the memory of each shard. Each shard reports
// plain numbers, and this shard builds the JSON objects.
future<json::json_return_type> get_shards(sharded<store>& db) {
    struct numbers {
        long keys, used_memory;
    };
    auto per_shard = co_await db.map([] (store& s) {
        return numbers{long(s.data.size()), long(s.used_memory)};
    });
    std::vector<aj::shard_info> shards;
    for (unsigned i = 0; i < per_shard.size(); i++) {
        aj::shard_info info;
        info.shard = i;
        info.keys = per_shard[i].keys;
        info.used_memory = per_shard[i].used_memory;
        shards.push_back(std::move(info));
    }
    co_return shards;
}

// snippet: get-keys
// GET /v1/keys?pattern=...: the matching keys, as a JSON array, which is
// written to the connection a key at a time, rather than built whole.
future<json::json_return_type> get_keys(sharded<store>& db,
                                        std::unique_ptr<http::request> req) {
    auto pattern = req->get_query_param("pattern", "*");
    auto keys = co_await matching_keys(db, std::move(pattern));
    co_return json::stream_range_as_array(std::move(keys),
                                          [] (const sstring& key) {
        return key;
    });
}
// end snippet: get-keys

// snippet: delete-key
// DELETE /v1/keys/{key}: deletes the key, or answers 404 Not Found.
future<json::json_return_type> remove_key(sharded<store>& db,
                                          std::unique_ptr<http::request> req) {
    auto key = req->get_path_param("key");
    if (!co_await delete_key(db, key)) {
        throw httpd::not_found_exception(fmt::format("no key '{}'", key));
    }
    co_return json::json_void();
}

// POST /v1/rewrite: starts a rewrite of the logs, or answers 409 Conflict
// if one is in progress.
future<json::json_return_type> rewrite(sharded<store>& db) {
    if (!co_await start_rewrite(db)) {
        throw httpd::base_exception("a rewrite is already in progress",
                                    http::reply::status_type::conflict);
    }
    co_return json::json_void();
}
// end snippet: delete-key

} // anonymous namespace

// snippet: set-routes
void set_admin_routes(httpd::routes& r, sharded<store>& db) {
    // Each lambda only calls a coroutine function, and passes it what it
    // needs, so that no coroutine refers to a lambda's captures.
    aj::get_info.set(r, [&db] (std::unique_ptr<http::request>) {
        return get_info(db);
    });
    aj::get_shards.set(r, [&db] (std::unique_ptr<http::request>) {
        return get_shards(db);
    });
    aj::get_keys.set(r, [&db] (std::unique_ptr<http::request> req) {
        return get_keys(db, std::move(req));
    });
    aj::delete_key.set(r, [&db] (std::unique_ptr<http::request> req) {
        return remove_key(db, std::move(req));
    });
    aj::rewrite.set(r, [&db] (std::unique_ptr<http::request>) {
        return rewrite(db);
    });
}
// end snippet: set-routes
