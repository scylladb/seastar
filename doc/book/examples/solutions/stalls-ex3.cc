// Chapter 22, exercise 3: clearing a vector gently, from its back, while a
// timer that should fire every millisecond notes how late it gets. Run it
// with one shard and 4 GB: -c1 -m4G.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <algorithm>
#include <chrono>
#include <print>
#include <string>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

// snippet: vector
// Empties a vector from its back, where removing an element moves no
// other, yielding as it goes. Then frees the vector's array, which is
// one deallocation.
template <typename T>
future<> clear_gently(std::vector<T>& v) {
    while (!v.empty()) {
        v.pop_back();
        co_await coroutine::maybe_yield();
    }
    v.shrink_to_fit();
}
// end snippet: vector

std::vector<std::string> make_strings() {
    std::vector<std::string> v;
    for (int i = 0; i < 10'000'000; i++) {
        v.push_back(std::string(40, 'x'));      // longer than kept inline
    }
    return v;
}

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
        for (bool gently : {false, true}) {
            auto v = make_strings();
            co_await sleep(10ms);
            worst = {};
            last = clk::now();
            heartbeat.arm_periodic(1ms);
            auto start = clk::now();
            if (gently) {
                co_await clear_gently(v);
            } else {
                v = {};
            }
            auto took = clk::now() - start;
            co_await sleep(10ms);
            heartbeat.cancel();
            std::println("{}: {:.0f} ms; the heartbeat was up to {:.1f} ms "
                         "late", gently ? "gently" : "at once",
                         ms(took).count(), ms(worst).count());
        }
    });
}
