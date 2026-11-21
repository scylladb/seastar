// The solution of chapter 31's exercise 1: an idle CPU handler that counts
// primes, a little at a time, while a timer that should fire every
// millisecond notes how late it comes; with the handler checking for work
// often, rarely, and not at all.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/idle_cpu_handler.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <chrono>
#include <cstdint>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

static bool is_prime(uint64_t n) {
    for (uint64_t d = 2; d * d <= n; d++) {
        if (n % d == 0) {
            return false;
        }
    }
    return n >= 2;
}

// snippet: handler
// Counts primes, from where it stopped, until the reactor has work again,
// asking it every `check` numbers. It always has more to do, so it never
// says no_more_work, and the reactor never sleeps.
struct prime_counter {
    uint64_t next = 2;
    uint64_t primes = 0;
    int check;

    idle_cpu_handler_result operator()(work_waiting_on_reactor poll) {
        while (true) {
            for (int i = 0; i < check; i++) {
                primes += is_prime(next++);
            }
            if (poll()) {
                return idle_cpu_handler_result::interrupted_by_higher_priority_task;
            }
        }
    }
};
// end snippet: handler

// Runs a 1 ms heartbeat for a second, with the handler set to check every
// `check` numbers (or with no handler, if 0), and prints how late it came.
future<> run(int check) {
    prime_counter counter{.check = check};
    if (check) {
        set_idle_cpu_handler([&counter] (work_waiting_on_reactor poll) {
            return counter(poll);
        });
    }
    auto last = clk::now();
    clk::duration worst{};
    timer<> heartbeat([&] {
        auto now = clk::now();
        worst = std::max(worst, now - last - 1ms);
        last = now;
    });
    heartbeat.arm_periodic(1ms);
    co_await sleep(1s);
    heartbeat.cancel();
    // A handler that has no more work lets the reactor sleep again.
    set_idle_cpu_handler([] (work_waiting_on_reactor) {
        return idle_cpu_handler_result::no_more_work;
    });
    using ms = std::chrono::duration<double, std::milli>;
    if (check) {
        std::println("checking every {:>9} numbers: {:>8} primes found, heartbeat up to "
                     "{:.2f} ms late", check, counter.primes, ms(worst).count());
    } else {
        std::println("no idle handler: heartbeat up to {:.2f} ms late", ms(worst).count());
    }
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await run(0);
        co_await run(100);
        co_await run(1'000'000);
    });
}
