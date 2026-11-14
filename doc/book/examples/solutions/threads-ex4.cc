// Chapter 21, exercise 4: deferred.cc's run() as a coroutine, which can't
// use deferred_stop, since a destructor can't wait for a future. It stops
// its services itself, in the reverse order, whichever way it ends.
// Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/sstring.hh>
#include <chrono>
#include <exception>
#include <print>
#include <stdexcept>

using namespace seastar;
using namespace std::chrono_literals;

// A service that takes a while to stop.
class service {
    sstring _name;
public:
    explicit service(sstring name) : _name(std::move(name)) {
        std::println("{} started", _name);
    }
    future<> stop() noexcept {
        co_await sleep(10ms);
        std::println("{} stopped", _name);
    }
};

// snippet: run
future<> run(bool fail) {
    service database("database");
    std::exception_ptr error;
    try {
        service server("server");
        try {
            if (fail) {
                throw std::runtime_error("something failed");
            }
            co_await sleep(100ms);      // serve, until it's time to stop
        } catch (...) {
            // No co_await in a catch block: keep the exception for later.
            error = std::current_exception();
        }
        co_await server.stop();
    } catch (...) {
        error = std::current_exception();   // starting the server failed
    }
    co_await database.stop();
    if (error) {
        std::rethrow_exception(error);
    }
}
// end snippet: run

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await run(false);
        try {
            co_await run(true);
        } catch (std::exception& e) {
            std::println("run: {}", e.what());
        }
    });
}
