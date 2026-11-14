// What a message to another shard costs: round trips with smp::submit_to(),
// one at a time, or many at once, from shard 0 to the shard given by --to.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/smp.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <chrono>
#include <cstdint>
#include <print>
#include <ranges>

using namespace seastar;
namespace bpo = boost::program_options;

// snippet: round-trips
// Asks shard `to` to run a function that does nothing, n times, waiting
// for each answer before sending the next request.
future<> round_trips(unsigned to, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) {
        co_await smp::submit_to(to, [] {});
    }
}
// end snippet: round-trips

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
        ("to", bpo::value<unsigned>()->default_value(1),
         "the shard to send the requests to")
        ("calls", bpo::value<uint64_t>()->default_value(1'000'000),
         "how many requests to send")
        ("in-flight", bpo::value<unsigned>()->default_value(1),
         "how many requests to have in flight at once");
    return app.run(argc, argv, [&app] (this auto) -> future<> {
        auto& opts = app.configuration();
        auto to = opts["to"].as<unsigned>();
        auto calls = opts["calls"].as<uint64_t>();
        auto in_flight = opts["in-flight"].as<unsigned>();
        auto start = std::chrono::steady_clock::now();
        // snippet: in-flight
        // in_flight fibers, each making its share of the round trips.
        co_await coroutine::parallel_for_each(
            std::views::iota(0u, in_flight), [&] (unsigned) {
                return round_trips(to, calls / in_flight);
            });
        // end snippet: in-flight
        std::chrono::duration<double> secs =
            std::chrono::steady_clock::now() - start;
        std::println("shard 0 to shard {}, {} in flight: {:.0f} ns per "
                     "request, {:.2f} million requests a second", to,
                     in_flight, secs.count() * 1e9 / calls,
                     calls / secs.count() / 1e6);
    });
}
