// Three scheduling groups compete for one shard's CPU, each with a loop
// that does a small piece of work, again and again, and counts the pieces.
// The counts show how the reactor divides the CPU: in proportion to the
// groups' shares, among the groups that have work to do.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <fmt/format.h>
#include <chrono>
#include <cstdint>
#include <print>
#include <ranges>
#include <string_view>
#include <vector>

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

// snippet: spin
// Does pieces of work until the time is up, and returns how many it did.
// maybe_yield() lets the reactor run other tasks, and other groups,
// whenever this one has run for a task quota.
future<uint64_t> spin(clk::time_point until) {
    uint64_t pieces = 0;
    uint64_t x = 1;
    while (clk::now() < until) {
        x = work(x);
        pieces++;
        co_await coroutine::maybe_yield();
    }
    co_return x ? pieces : pieces + 1;  // uses x, so it can't be dropped
}
// end snippet: spin

// snippet: compete
// Runs spin() in each of the groups, for the same second, and prints each
// group's part of all the pieces done.
future<> compete(std::string_view title,
                 std::vector<scheduling_group> groups) {
    auto until = clk::now() + 1s;
    std::vector<uint64_t> done(groups.size());
    co_await coroutine::parallel_for_each(
            std::views::iota(size_t(0), groups.size()),
            [&] (size_t i) -> future<> {
        done[i] = co_await with_scheduling_group(groups[i], spin, until);
    });
    uint64_t total = 0;
    for (auto n : done) {
        total += n;
    }
    std::string line;
    for (size_t i = 0; i < groups.size(); i++) {
        line += fmt::format("  {} {:.0f}%", groups[i].name(),
                            100.0 * done[i] / total);
    }
    std::println("{}:{}", title, line);
}
// end snippet: compete

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: groups
        auto high = co_await create_scheduling_group("high", 1000);
        auto medium = co_await create_scheduling_group("medium", 500);
        auto low = co_await create_scheduling_group("low", 250);
        co_await compete("all three busy", {high, medium, low});
        co_await compete("high idle", {medium, low});
        // end snippet: groups
        for (auto sg : {high, medium, low}) {
            co_await destroy_scheduling_group(sg);
        }
    });
}
