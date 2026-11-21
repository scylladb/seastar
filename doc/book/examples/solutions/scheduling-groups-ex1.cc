// Exercise 1 of "Scheduling groups": cpu-shares.cc, with medium and low
// in a supergroup of 1,000 shares, and high alone. The supergroup competes
// with high for the shard, and its groups divide the supergroup's part.
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

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        auto high = co_await create_scheduling_group("high", 1000);
        auto super = co_await create_scheduling_supergroup(1000);
        auto medium = co_await create_scheduling_group("medium", "med",
                                                       500, super);
        auto low = co_await create_scheduling_group("low", "low", 250,
                                                    super);
        co_await compete("all three busy", {high, medium, low});
        co_await compete("high idle", {medium, low});
        for (auto sg : {high, medium, low}) {
            co_await destroy_scheduling_group(sg);
        }
        co_await destroy_scheduling_supergroup(super);
    });
}
