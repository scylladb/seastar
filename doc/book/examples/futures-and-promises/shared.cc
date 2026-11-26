// Many coroutines waiting for one result: a shared_future that loads each
// value only once, and a shared_promise that announces an event.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/sstring.hh>
#include <fmt/format.h>
#include <chrono>
#include <print>
#include <unordered_map>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: loader
// Gives the value of a key. Loading a value is slow, so the loader loads
// each key only once, however many coroutines ask for it, and when they ask.
class loader {
    std::unordered_map<sstring, shared_future<sstring>> _values;
    int _loads = 0;

    future<sstring> load(sstring key) {
        ++_loads;
        co_await sleep(100ms);    // reading it from a disk, say
        co_return fmt::format("the value of {}", key);
    }
public:
    future<sstring> get(const sstring& key) {
        auto it = _values.find(key);
        if (it == _values.end()) {
            it = _values.emplace(key, load(key)).first;
        }
        return it->second.get_future();
    }
    int loads() const {
        return _loads;
    }
};
// end snippet: loader

// snippet: ask
future<> ask(loader& l, int who, sstring key) {
    auto value = co_await l.get(key);
    std::println("{} got {}", who, value);
}
// end snippet: ask

// snippet: wait-until-ready
future<> wait_until_ready(shared_promise<>& ready, int who) {
    co_await ready.get_shared_future();
    std::println("{} is running", who);
}
// end snippet: wait-until-ready

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: main
        loader l;
        auto start = std::chrono::steady_clock::now();
        std::vector<future<>> asks;
        for (int who = 1; who <= 4; who++) {
            asks.push_back(ask(l, who, who % 2 ? "x" : "y"));
        }
        for (auto& f : asks) {
            co_await std::move(f);
        }
        co_await ask(l, 5, "x");
        std::println("{} loads in {}", l.loads(),
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start));
        // end snippet: main

        // snippet: ready
        shared_promise<> ready;
        std::vector<future<>> waiters;
        for (int who = 1; who <= 3; who++) {
            waiters.push_back(wait_until_ready(ready, who));
        }
        std::println("starting up");
        co_await sleep(100ms);
        ready.set_value();
        for (auto& f : waiters) {
            co_await std::move(f);
        }
        // end snippet: ready
    });
}
