// The solution of chapter 25's exercise 4: a client that deletes 1,000 keys
// from the program's own server, a hundred requests at a time, and counts
// the connections that it opened, with the client's default pool, and with
// a pool of ten connections. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/loop.hh>
#include <seastar/http/client.hh>
#include <seastar/http/exception.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/request.hh>
#include <memory>
#include <print>
#include <ranges>
#include <set>

using namespace seastar;

constexpr uint16_t port = 10081;
constexpr unsigned n_keys = 1000;

// The server's keys, "key0" to "key999", which DELETE /keys/{key} deletes.
std::set<sstring> keys;

void set_routes(httpd::routes& r) {
    r.add(httpd::operation_type::DELETE, httpd::url("/keys").remainder("key"),
          new httpd::function_handler([] (httpd::const_req req) {
        auto key = req.get_path_param("key");
        if (!keys.erase(key)) {
            throw httpd::not_found_exception(fmt::format("no key '{}'", key));
        }
        return sstring();
    }, "txt"));
}

// snippet: delete-all
// Deletes one key, and fails unless the server answers 200 OK.
future<> delete_key(http::client& client, unsigned i) {
    auto req = http::request::make("DELETE", "localhost",
                                   fmt::format("/keys/key{}", i));
    co_await client.make_request(std::move(req),
            [] (const http::reply&, input_stream<char>&&) {
        return make_ready_future();
    }, http::reply::status_type::ok);
}

// Deletes all the keys, with at most a hundred requests at a time.
future<> delete_all(http::client& client) {
    co_await max_concurrent_for_each(std::views::iota(0u, n_keys), 100,
            [&client] (unsigned i) {
        return delete_key(client, i);
    });
}
// end snippet: delete-all

// snippet: run
// Fills the server's keys, deletes them all with the client, and prints the
// connections that the client opened.
future<> run(http::client& client, const char* name) {
    for (unsigned i = 0; i < n_keys; i++) {
        keys.insert(fmt::format("key{}", i));
    }
    co_await delete_all(client);
    std::println("{}: {} keys left, {} connections opened", name,
                 keys.size(), client.total_new_connections_nr());
    co_await client.close();
}
// end snippet: run

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        auto address = socket_address(net::inet_address("127.0.0.1"), port);
        httpd::http_server_control server;
        co_await server.start("keys");
        co_await server.set_routes(set_routes);
        std::exception_ptr error;
        try {
            co_await server.listen(address);
            // snippet: clients
            http::client default_pool(address);
            co_await run(default_pool, "default pool");
            http::client ten(
                std::make_unique<http::basic_connection_factory>(address), 10);
            co_await run(ten, "pool of 10");
            // end snippet: clients
        } catch (...) {
            error = std::current_exception();
        }
        co_await server.stop();
        if (error) {
            std::rethrow_exception(error);
        }
    });
}
