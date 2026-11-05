// Destroying a large container without a stall: at once, and gradually,
// yielding as it goes, while a timer that should fire every millisecond
// notes how late it gets. Run it with one shard and 4 GB: -c1 -m4G.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <algorithm>
#include <chrono>
#include <memory>
#include <print>
#include <random>
#include <string>
#include <unordered_map>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;
using table = std::unordered_map<int, std::string>;

// snippet: clear-gently
// Empties a container, a few elements at a time, letting other tasks run
// in between. Its erase() and begin() must be cheap, as they are for the
// standard containers that are linked lists of nodes.
template <typename Container>
future<> clear_gently(Container& c) {
    while (!c.empty()) {
        c.erase(c.begin());
        co_await coroutine::maybe_yield();
    }
}
// end snippet: clear-gently

// Two million keys, at random, each with a string of 100 characters.
std::unique_ptr<table> make_table() {
    auto t = std::make_unique<table>();
    std::mt19937 random(1);
    while (t->size() < 2'000'000) {
        t->emplace(random(), std::string(100, 'x'));
    }
    return t;
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // A heartbeat, every millisecond, which notes how late it came.
        auto last = clk::now();
        clk::duration worst{};
        timer<> heartbeat([&] {
            auto now = clk::now();
            worst = std::max(worst, now - last - 1ms);
            last = now;
        });
        using ms = std::chrono::duration<double, std::milli>;
        // snippet: compare
        auto t = make_table();
        co_await sleep(10ms);
        worst = {};
        last = clk::now();
        heartbeat.arm_periodic(1ms);
        t.reset();                              // at once
        co_await sleep(10ms);
        std::println("at once: the heartbeat was up to {:.1f} ms late",
                     ms(worst).count());

        heartbeat.cancel();
        t = make_table();
        co_await sleep(10ms);
        worst = {};
        last = clk::now();
        heartbeat.arm_periodic(1ms);
        auto start = clk::now();
        co_await clear_gently(*t);              // gradually
        std::println("gradually, in {:.0f} ms: the heartbeat was up to "
                     "{:.1f} ms late", ms(clk::now() - start).count(),
                     ms(worst).count());
        // end snippet: compare
        heartbeat.cancel();
    });
}
