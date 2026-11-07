// A second of computation, done in one of several ways, while a heartbeat
// timer measures how long the rest of the shard has to wait.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/util/later.hh>
#include <boost/program_options.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <print>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using namespace seastar;
using namespace std::chrono_literals;
namespace bpo = boost::program_options;
using clock_type = std::chrono::steady_clock;

// snippet: heartbeat
// Wakes up every millisecond, until told to stop, and measures how late it
// woke up.
future<> heartbeat(const bool& stop, clock_type::duration& latest,
                   int& beats) {
    while (!stop) {
        auto expected = clock_type::now() + 1ms;
        co_await sleep(1ms);
        latest = std::max(latest, clock_type::now() - expected);
        beats++;
    }
}
// end snippet: heartbeat

// About a microsecond of computation.
uint64_t compute(uint64_t x) {
    for (int i = 0; i < 400; i++) {
        x ^= x >> 31;
        x *= 0x9e3779b97f4a7c15;
    }
    return x;
}

// The same, as a coroutine that never waits.
future<uint64_t> compute_coroutine(uint64_t x) {
    co_return compute(x);
}

uint64_t result;    // keeps the compiler from skipping the computation

// snippet: work
// Computes until the deadline, and returns how many times it called
// compute(). How it lets the rest of the shard run depends on "how".
future<uint64_t> work(std::string how, clock_type::time_point deadline) {
    uint64_t x = 1, n = 0;
    while (clock_type::now() < deadline) {
        if (how == "plain") {
            x = compute(x);
        } else if (how == "maybe_yield") {
            x = compute(x);
            co_await coroutine::maybe_yield();
        } else if (how == "yield") {
            x = compute(x);
            co_await yield();
        } else if (how == "coroutine") {
            x = co_await compute_coroutine(x);
        } else if (how == "without_preemption_check") {
            auto f = compute_coroutine(x);
            x = co_await coroutine::without_preemption_check(std::move(f));
        } else if (how == "block") {
            std::this_thread::sleep_for(1s);
        } else {
            throw std::invalid_argument("unknown --work: " + how);
        }
        n++;
    }
    result = x;
    co_return n;
}
// end snippet: work

future<> experiment(std::string how) {
    bool stop = false;
    clock_type::duration latest{};
    int beats = 0;
    auto beating = heartbeat(stop, latest, beats);
    // Let the heartbeat run for a moment before the work starts. (Seastar's
    // stall detector misses a stall in the first batch of tasks that the
    // reactor runs.)
    co_await sleep(10ms);
    latest = {};
    beats = 0;
    auto start = clock_type::now();
    auto n = co_await work(how, start + 1s);
    auto took = clock_type::now() - start;
    stop = true;
    co_await std::move(beating);
    auto late_ms = std::chrono::duration<double, std::milli>(latest).count();
    std::println("{}: {} computations in {} ms; {} heartbeats, "
                 "the latest {:.2f} ms late",
                 how, n, took / 1ms, beats, late_ms);
}

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
        ("work", bpo::value<std::string>()->default_value("plain"),
         "how to do the work: plain, maybe_yield, yield, coroutine, "
         "without_preemption_check or block");
    return app.run(argc, argv, [&app] {
        return experiment(app.configuration()["work"].as<std::string>());
    });
}
