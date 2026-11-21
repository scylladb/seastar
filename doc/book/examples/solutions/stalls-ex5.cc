// Chapter 22, exercise 5: sorting ten million numbers without a stall: in
// pieces, and then merging them, yielding as it goes, against
// std::ranges::sort(), while a timer that should fire every millisecond
// notes how late it gets. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <print>
#include <random>
#include <utility>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

// snippet: sort
// Sorts v in pieces of up to `piece` elements, yielding after each, and
// then merges pairs of sorted runs, of twice the length each time, into a
// second array, yielding as it goes. The pieces are small enough to sort
// within a task quota, and the second array is only reserved, not filled
// with zeros: its pages are first touched a little at a time, as the
// merge writes them.
future<> sort_gently(std::vector<int>& v, size_t piece = 10'000) {
    for (size_t i = 0; i < v.size(); i += piece) {
        std::ranges::sort(v.begin() + i,
                          v.begin() + std::min(i + piece, v.size()));
        co_await coroutine::maybe_yield();
    }
    std::vector<int> out;
    out.reserve(v.size());
    for (size_t run = piece; run < v.size(); run *= 2) {
        out.clear();
        for (size_t start = 0; start < v.size(); start += 2 * run) {
            size_t mid = std::min(start + run, v.size());
            size_t end = std::min(start + 2 * run, v.size());
            size_t a = start, b = mid;
            while (a < mid || b < end) {
                bool from_a = b == end || (a < mid && v[a] <= v[b]);
                out.push_back(from_a ? v[a++] : v[b++]);
                if (out.size() % 4096 == 0) {
                    co_await coroutine::maybe_yield();
                }
            }
        }
        std::swap(v, out);
    }
}
// end snippet: sort

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
            std::vector<int> v(10'000'000);
            std::mt19937 random(1);
            for (auto& n : v) {
                n = random();
            }
            co_await sleep(10ms);
            worst = {};
            last = clk::now();
            heartbeat.arm_periodic(1ms);
            auto start = clk::now();
            if (gently) {
                co_await sort_gently(v);
            } else {
                std::ranges::sort(v);
            }
            auto took = clk::now() - start;
            co_await sleep(10ms);
            heartbeat.cancel();
            std::println("{}: {:.0f} ms, sorted: {}; the heartbeat was "
                         "up to {:.1f} ms late",
                         gently ? "in pieces" : "at once",
                         ms(took).count(), std::ranges::is_sorted(v),
                         ms(worst).count());
        }
    });
}
