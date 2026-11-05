// Chapter 8, exercise 3: ptr-cost.cc, with use() an ordinary function,
// which the compiler can inline, and see that it does nothing with its
// pointer. Run with --variant raw, lw, seastar, std or std-shared.
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

// Takes a pointer by value, and does nothing with it.
template <typename Ptr>
void use(Ptr p) {
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
        std::chrono::duration<double> secs =
            std::chrono::steady_clock::now() - start;
        std::println("{} shards, {}: {:.2f} ns per call",
                     this_smp_shard_count(), variant,
                     secs.count() * 1e9 / calls);
        shared_a.reset();
    });
}
