// What passing a shared pointer to a function by value costs: the copy
// that the call makes increments the pointer's count, and destroying the
// copy, after the call, decrements it. Run with --variant raw (a plain
// pointer, which has no count, for comparison), lw (lw_shared_ptr),
// seastar (seastar::shared_ptr), std (std::shared_ptr), or std-shared
// (std::shared_ptr, with every shard passing copies of the same pointer).
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/smp.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <chrono>
#include <cstdint>
#include <memory>
#include <print>
#include <string>

using namespace seastar;
namespace bpo = boost::program_options;

// snippet: calls
// Takes a pointer by value, and does nothing with it. noinline, and the
// empty asm statement, which tells the compiler that p's address is used
// by code it can't see, keep the compiler from noticing that it does
// nothing, and leaving out the copy, with its increment and decrement.
template <typename Ptr>
[[gnu::noinline]] void use(Ptr p) {
    asm volatile("" : : "r"(&p) : "memory");
}

template <typename Ptr>
future<> pass_many(Ptr a, uint64_t calls) {
    for (uint64_t i = 0; i < calls; i++) {
        use(a);
        if (i % 1000 == 0) {
            co_await coroutine::maybe_yield();
        }
    }
}
// end snippet: calls

// The object whose pointer every shard passes, in the std-shared variant.
std::shared_ptr<int> shared_a;

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
        ("variant", bpo::value<std::string>()->default_value("lw"),
         "raw, lw, seastar, std or std-shared")
        ("calls", bpo::value<uint64_t>()->default_value(100'000'000),
         "calls on each shard");
    return app.run(argc, argv, [&app] (this auto) -> future<> {
        auto variant = app.configuration()["variant"].as<std::string>();
        auto calls = app.configuration()["calls"].as<uint64_t>();
        shared_a = std::make_shared<int>(1);
        auto start = std::chrono::steady_clock::now();
        co_await smp::invoke_on_all([&] () -> future<> {
            if (variant == "raw") {
                int x = 1;
                co_await pass_many(&x, calls);
            } else if (variant == "lw") {
                co_await pass_many(make_lw_shared<int>(1), calls);
            } else if (variant == "seastar") {
                co_await pass_many(seastar::make_shared<int>(1), calls);
            } else if (variant == "std") {
                co_await pass_many(std::make_shared<int>(1), calls);
            } else if (variant == "std-shared") {
                co_await pass_many(shared_a, calls);
            } else {
                throw std::invalid_argument("unknown --variant");
            }
        });
        std::chrono::duration<double> secs = std::chrono::steady_clock::now() - start;
        std::println("{} shards, {}: {:.2f} ns per call", this_smp_shard_count(), variant,
                     secs.count() * 1e9 / calls);
        shared_a.reset();
    });
}
