// Chapter 23, exercise 3: counting down with continuations, by recursion,
// and with repeat(), while a timer that should fire every millisecond
// notes how late it gets. The recursion runs only a hundred thousand
// levels: ten million crash the program, in an optimized build. Run it with
// one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <algorithm>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

// snippet: count-down
// Calls itself in the continuation of an available future, which an
// optimized build calls at once, on the stack, n levels deep.
future<> count_down(int n) {
    if (n == 0) {
        return make_ready_future<>();
    }
    return make_ready_future<>().then([n] {
        return count_down(n - 1);
    });
}

// The same, with repeat(), which runs each turn after the previous one
// ends, and checks for preemption between them.
future<> count_down_repeat(int n) {
    return repeat([n] () mutable {
        return make_ready_future<stop_iteration>(
            --n == 0 ? stop_iteration::yes : stop_iteration::no);
    });
}
// end snippet: count-down

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        auto last = clk::now();
        clk::duration worst{};
        timer<> heartbeat([&] {
            auto now = clk::now();
            worst = std::max(worst, now - last - 1ms);
            last = now;
        });
        using ms = std::chrono::duration<double, std::milli>;
        auto run = [&] (auto name, auto f) -> future<> {
            co_await sleep(10ms);
            worst = {};
            last = clk::now();
            heartbeat.arm_periodic(1ms);
            auto start = clk::now();
            co_await f();
            auto took = clk::now() - start;
            co_await sleep(10ms);
            heartbeat.cancel();
            std::println("{}: {:.1f} ms; the heartbeat was up to {:.1f} ms "
                         "late", name, ms(took).count(), ms(worst).count());
        };
        co_await run("recursion, 100,000",
                     [] { return count_down(100'000); });
        co_await run("repeat(), 10,000,000",
                     [] { return count_down_repeat(10'000'000); });
    });
}
