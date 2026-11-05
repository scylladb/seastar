// What a semaphore costs: taking and giving back a unit when it's free,
// and passing a unit back and forth between two fibers that wait for it.
// A std::mutex, which a Seastar shard never needs, for comparison.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/later.hh>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <print>
#include <string_view>

using namespace seastar;
using clock_type = std::chrono::steady_clock;

// Few enough that the loops that never yield stay well below the stall
// detector's 25 milliseconds.
constexpr int64_t n = 1'000'000;

void report(std::string_view what, clock_type::duration took, int64_t ops) {
    std::println("{}: {:.1f} ns", what,
                 std::chrono::duration<double, std::nano>(took).count() / ops);
}

future<> uncontended() {
    semaphore sem(1);
    auto start = clock_type::now();
    for (int64_t i = 0; i < n; i++) {
        auto held = co_await get_units(sem, 1);
    }
    report("get_units(), free", clock_type::now() - start, n);
    start = clock_type::now();
    for (int64_t i = 0; i < n; i++) {
        auto held = try_get_units(sem, 1);
    }
    report("try_get_units(), free", clock_type::now() - start, n);
    std::mutex m;
    start = clock_type::now();
    for (int64_t i = 0; i < n; i++) {
        std::lock_guard held(m);
    }
    report("std::mutex, free", clock_type::now() - start, n);
}

// Two fibers take turns: each takes the unit, and gives it back after
// yielding, while the other one waits for it.
future<> take_turns(semaphore& sem, int64_t turns) {
    for (int64_t i = 0; i < turns; i++) {
        auto held = co_await get_units(sem, 1);
        co_await yield();
    }
}

future<> contended() {
    semaphore sem(1);
    constexpr int64_t turns = 1'000'000;
    auto start = clock_type::now();
    co_await when_all(take_turns(sem, turns), take_turns(sem, turns));
    report("a unit passed between two fibers, per turn",
           clock_type::now() - start, 2 * turns);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await uncontended();
        co_await contended();
    });
}
