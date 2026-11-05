// The cost of sharing: every shard increments a counter, either one of its
// own or one that all the shards share. Both are std::atomic, so the only
// difference is whether other shards use the same counter.
// Run with --variant own or shared.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/smp.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <print>
#include <string>

using namespace seastar;
namespace bpo = boost::program_options;

// snippet: counters
// One counter for the whole program, which every shard increments.
std::atomic<uint64_t> shared_counter;

// One counter for each shard: a thread_local variable has a copy in each
// thread, and each shard is a thread of its own.
thread_local std::atomic<uint64_t> own_counter;
// end snippet: counters

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
        ("variant", bpo::value<std::string>()->default_value("own"), "own or shared")
        ("increments", bpo::value<uint64_t>()->default_value(100'000'000),
         "increments on each shard");
    return app.run(argc, argv, [&app] () -> future<> {
        auto variant = app.configuration()["variant"].as<std::string>();
        auto n = app.configuration()["increments"].as<uint64_t>();
        if (variant != "own" && variant != "shared") {
            throw std::invalid_argument("--variant must be own or shared");
        }
        auto start = std::chrono::steady_clock::now();
        // snippet: increment
        co_await smp::invoke_on_all([&] () -> future<> {
            auto& counter = variant == "own" ? own_counter : shared_counter;
            for (uint64_t i = 0; i < n; i++) {
                counter.fetch_add(1, std::memory_order_relaxed);
                co_await coroutine::maybe_yield();
            }
        });
        // end snippet: increment
        std::chrono::duration<double> secs = std::chrono::steady_clock::now() - start;
        std::println("{} shards, {} counters: {:.2f} s, {:.1f} ns per increment",
                     this_smp_shard_count(), variant, secs.count(), secs.count() * 1e9 / n);
    });
}
