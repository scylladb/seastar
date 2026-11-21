// Chapter 21, exercise 3: what alien::run_on() costs. An alien thread sends
// 100,000 functions to shard 0, each incrementing a counter; the program
// measures how long sending took, and how long until all of them ran.
// Run it with two shards: -c2.
#include <seastar/core/alien.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <atomic>
#include <chrono>
#include <print>
#include <thread>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

constexpr int n = 100'000;

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [&app] () -> future<> {
        // snippet: send
        alien::instance& seastar = app.alien();
        int count = 0;                      // only shard 0 touches it
        std::atomic<bool> sent = false;
        clk::duration sending{};
        auto start = clk::now();
        std::thread outside([&] {
            for (int i = 0; i < n; i++) {
                alien::run_on(seastar, 0, [&count] () noexcept { count++; });
            }
            sending = clk::now() - start;
            sent = true;
        });
        while (count < n) {
            co_await sleep(100us);
        }
        auto all = clk::now() - start;
        while (!sent) {
            co_await sleep(100us);
        }
        outside.join();
        // end snippet: send
        using ns = std::chrono::duration<double, std::nano>;
        std::println("sending: {:.0f} ns each; until all ran: {:.0f} ns",
                     ns(sending).count() / n, ns(all).count() / n);
    });
}
