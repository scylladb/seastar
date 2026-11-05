// Changing the stall detector's threshold while the program runs: a
// computation of about 15 ms, which the default threshold of 25 ms
// doesn't report, and a threshold of 5 ms does. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// Keeps the shard busy for about this long, without yielding.
void busy(std::chrono::milliseconds how_long) {
    auto end = std::chrono::steady_clock::now() + how_long;
    while (std::chrono::steady_clock::now() < end) {
    }
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: threshold
        for (auto threshold : {25ms, 5ms}) {
            engine().update_blocked_reactor_notify_ms(threshold);
            // To stderr, where the stall detector's reports go, so that
            // the two come out in order.
            std::println(stderr, "threshold {} ms: working for 15 ms",
                         engine().get_blocked_reactor_notify_ms().count());
            co_await sleep(10ms);
            busy(15ms);
            co_await sleep(10ms);
        }
        // end snippet: threshold
    });
}
