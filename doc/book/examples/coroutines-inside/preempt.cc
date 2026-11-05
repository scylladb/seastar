// Why every co_await is a preemption point: a loop that awaits fifty
// million available futures, normally and with without_preemption_check,
// while a timer that should fire every millisecond notes how late it gets.
// Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <algorithm>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

[[gnu::noinline]] future<int> ready(int i) {
    return make_ready_future<int>(i & 1);
}

// snippet: loops
// Awaits n available futures. Each co_await checks need_preempt(), and
// yields if the task quota is used up.
future<long> await_all(int n) {
    long sum = 0;
    for (int i = 0; i < n; i++) {
        sum += co_await ready(i);
    }
    co_return sum;
}

// The same, without the check: it never yields.
future<long> await_all_without_check(int n) {
    long sum = 0;
    for (int i = 0; i < n; i++) {
        sum += co_await coroutine::without_preemption_check(ready(i));
    }
    co_return sum;
}
// end snippet: loops

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
        for (bool check : {true, false}) {
            co_await sleep(10ms);
            worst = {};
            last = clk::now();
            heartbeat.arm_periodic(1ms);
            auto start = clk::now();
            long sum = check ? co_await await_all(50'000'000)
                             : co_await await_all_without_check(50'000'000);
            auto took = clk::now() - start;
            co_await sleep(10ms);
            heartbeat.cancel();
            std::println("{}: sum {} in {:.0f} ms; the heartbeat was up to "
                         "{:.1f} ms late",
                         check ? "co_await" : "without_preemption_check",
                         sum, ms(took).count(), ms(worst).count());
        }
    });
}
