// Chapter 24, exercise 2: long-work.cc's count_primes() as a coroutine,
// which yields with coroutine::maybe_yield(), and then with the test of
// one number in a function of its own, which yields too, so it must be a
// coroutine. Each runs while a heartbeat notes how late it came, after
// the chapter's version, in a Seastar thread, for comparison.
// Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/timer.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <algorithm>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

// The chapter's version, for a Seastar thread.
int count_primes_in_thread(int n) {
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
        thread::maybe_yield();
    }
    return count;
}

// snippet: flat
// The same loops in a coroutine: the yield is in the coroutine itself.
future<int> count_primes(int n) {
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
        co_await coroutine::maybe_yield();
    }
    co_return count;
}
// end snippet: flat

// snippet: split
// The test of one number, in a function of its own that yields: it can
// wait only by suspending, so it must be a coroutine, and so must its
// caller, which waits for it.
future<bool> is_prime(int i) {
    bool prime = true;
    for (int d = 2; d * d <= i; d++) {
        if (i % d == 0) {
            prime = false;
            break;
        }
    }
    co_await coroutine::maybe_yield();
    co_return prime;
}

future<int> count_primes_split(int n) {
    int count = 0;
    for (int i = 2; i < n; i++) {
        count += co_await is_prime(i);
    }
    co_return count;
}
// end snippet: split

// Runs count() while a heartbeat, every millisecond, notes how much later
// than that it came, and prints what it found.
template <typename Count>
future<> measure(const char* name, Count count) {
    auto last = clk::now();
    clk::duration worst{};
    timer<> heartbeat([&] {
        auto now = clk::now();
        worst = std::max(worst, now - last - 1ms);
        last = now;
    });
    heartbeat.arm_periodic(1ms);
    auto start = clk::now();
    int primes = co_await count();
    auto took = clk::now() - start;
    co_await sleep(2ms);        // let the heartbeat come once more
    using ms = std::chrono::duration<double, std::milli>;
    std::println("{}: {} primes in {:.0f} ms; the heartbeat was up to "
                 "{:.1f} ms late", name, primes, ms(took).count(),
                 ms(worst).count());
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        constexpr int n = 5'000'000;
        co_await measure("in a Seastar thread", [] {
            return async([] { return count_primes_in_thread(n); });
        });
        co_await measure("in a coroutine", [] { return count_primes(n); });
        co_await measure("with a coroutine for each number", [] {
            return count_primes_split(n);
        });
    });
}
