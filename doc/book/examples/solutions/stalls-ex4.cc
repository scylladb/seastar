// Chapter 22, exercise 4: which work does the stall detector report, with a
// threshold of 5 ms? Works for 4, 5, ... 14 ms, and lets the reports show
// which. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

void busy(std::chrono::milliseconds how_long) {
    auto end = std::chrono::steady_clock::now() + how_long;
    while (std::chrono::steady_clock::now() < end) {
    }
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: sweep
        engine().update_blocked_reactor_notify_ms(5ms);
        for (int ms = 4; ms <= 14; ms++) {
            std::println(stderr, "working for {} ms", ms);
            co_await sleep(20ms);
            busy(std::chrono::milliseconds(ms));
            co_await sleep(20ms);
        }
        // end snippet: sweep
    });
}
