// Long synchronous code in a Seastar thread: a computation that runs for
// about a second, without yielding, and with thread::maybe_yield(), while
// a timer that should fire every millisecond measures how late it gets.
// Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/timer.hh>
#include <algorithm>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

// snippet: count-primes
// Ordinary synchronous code: counts the primes below n, the slow way. With
// yield, it gives other tasks a chance every so often, which only code in
// a Seastar thread can do.
int count_primes(int n, bool yield) {
    int count = 0;
    for (int i = 2; i < n; i++) {
        bool prime = true;
        for (int d = 2; d * d <= i; d++) {
            if (i % d == 0) {
                prime = false;
                break;
            }
        }
        count += prime;
        if (yield) {
            thread::maybe_yield();
        }
    }
    return count;
}
// end snippet: count-primes

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        return async([] {
            for (bool yield : {false, true}) {
                // snippet: heartbeat
                // A heartbeat, every millisecond, which notes how much
                // later than that it came.
                auto last = clk::now();
                clk::duration worst{};
                timer<> heartbeat([&] {
                    auto now = clk::now();
                    worst = std::max(worst, now - last - 1ms);
                    last = now;
                });
                heartbeat.arm_periodic(1ms);
                auto start = clk::now();
                int primes = count_primes(5'000'000, yield);
                auto took = clk::now() - start;
                sleep(2ms).get();       // let the heartbeat come once more
                // end snippet: heartbeat
                using ms = std::chrono::duration<double, std::milli>;
                std::println("{}: {} primes in {:.0f} ms; the heartbeat "
                             "was up to {:.1f} ms late",
                             yield ? "yielding" : "not yielding", primes,
                             ms(took).count(), ms(worst).count());
            }
        });
    });
}
