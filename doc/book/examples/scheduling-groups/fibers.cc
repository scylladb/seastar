// One loop competes with ten loops for one shard's CPU: first in the same
// scheduling group, where the reactor runs their tasks in the order in
// which they become ready, and then in two groups with equal shares.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <chrono>
#include <cstdint>
#include <print>
#include <ranges>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

// A small piece of work: a few microseconds of computation.
uint64_t work(uint64_t x) {
    for (int i = 0; i < 5000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
    }
    return x;
}

// Does pieces of work until the time is up, and adds them to pieces.
future<> spin(clk::time_point until, uint64_t& pieces) {
    uint64_t x = 1;
    while (clk::now() < until) {
        x = work(x);
        pieces++;
        co_await coroutine::maybe_yield();
    }
    pieces += x == 0;   // uses x, so the work can't be dropped
}

// snippet: loops
// Runs n loops of spin() at once, in the current scheduling group, and
// returns the pieces of work that they did together.
future<uint64_t> loops(unsigned n, clk::time_point until) {
    uint64_t pieces = 0;
    co_await coroutine::parallel_for_each(std::views::iota(0u, n),
            [&] (unsigned) {
        return spin(until, pieces);
    });
    co_return pieces;
}

// One loop against ten, for a second, in the groups a and b.
future<> compete(const char* title, scheduling_group a, scheduling_group b) {
    auto until = clk::now() + 1s;
    uint64_t one = 0, ten = 0;
    co_await coroutine::parallel_for_each(std::views::iota(0, 2),
            [&] (int i) -> future<> {
        if (i == 0) {
            one = co_await with_scheduling_group(a, loops, 1, until);
        } else {
            ten = co_await with_scheduling_group(b, loops, 10, until);
        }
    });
    std::println("{}: one loop {:.0f}%, ten loops {:.0f}%", title,
                 100.0 * one / (one + ten), 100.0 * ten / (one + ten));
}
// end snippet: loops

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: compete
        auto group = current_scheduling_group();
        co_await compete("one group", group, group);
        auto a = co_await create_scheduling_group("a", 100);
        auto b = co_await create_scheduling_group("b", 100);
        co_await compete("two groups", a, b);
        // end snippet: compete
        co_await destroy_scheduling_group(a);
        co_await destroy_scheduling_group(b);
    });
}
