// Stopping services in a Seastar thread, with deferred_stop: they stop in
// the reverse order of starting, whichever way the function ends.
// Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/closeable.hh>
#include <chrono>
#include <print>
#include <stdexcept>

using namespace seastar;
using namespace std::chrono_literals;

// A service that takes a while to stop. stop() must not throw, since
// deferred_stop calls it in a destructor.
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

// snippet: deferred
void run(bool fail) {
    service database("database");
    auto stop_database = deferred_stop(database);
    service server("server");
    auto stop_server = deferred_stop(server);
    if (fail) {
        throw std::runtime_error("something failed");
    }
    sleep(100ms).get();         // serve, until it's time to stop
}
// end snippet: deferred

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        return async([] {
            run(false);
            try {
                run(true);
            } catch (std::exception& e) {
                std::println("run: {}", e.what());
            }
        });
    });
}
