// Chapter 14, exercise 4: nested.cc's calls in one smp_service_group,
// with --units units for calls between two shards, and --calls calls
// that each shard starts at once. Run with two shards.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/util/log.hh>
#include <fmt/format.h>
#include <chrono>
#include <ranges>
#include <stdexcept>

using namespace seastar;
using namespace std::chrono_literals;
namespace bpo = boost::program_options;

// Asks the other shard to work for a moment, and then to ask this shard,
// in turn, to do nothing, all in one group; a nested call that waits a
// second for its turn fails.
future<> call_and_call_back(smp_service_group group) {
    unsigned me = this_shard_id();
    unsigned other = 1 - me;
    co_await smp::submit_to(other, group,
            [me, group] (this auto) -> future<> {
        co_await sleep(10ms);
        auto deadline = lowres_clock::now() + 1s;
        co_await smp::submit_to(me, smp_submit_to_options(group, deadline),
                                [] {});
    });
}

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
        ("units", bpo::value<unsigned>()->default_value(2),
         "the group's max_nonlocal_requests")
        ("calls", bpo::value<unsigned>()->default_value(1),
         "how many calls each shard starts at once");
    return app.run(argc, argv, [&app] (this auto) -> future<> {
        if (this_smp_shard_count() != 2) {
            throw std::runtime_error("run with two shards (-c2)");
        }
        auto units = app.configuration()["units"].as<unsigned>();
        auto calls = app.configuration()["calls"].as<unsigned>();
        smp_service_group_config config;
        config.max_nonlocal_requests = units;
        auto group = co_await create_smp_service_group(config);
        auto f = co_await coroutine::as_future(smp::invoke_on_all(
            [group, calls] {
                return parallel_for_each(
                    std::views::iota(0u, calls),
                    [group] (unsigned) {
                        return call_and_call_back(group);
                    });
            }));
        if (f.failed()) {
            fmt::print("{} units, {} calls at once: {}\n", units, calls,
                       formattable(f.get_exception()));
        } else {
            fmt::print("{} units, {} calls at once: done\n", units, calls);
        }
        co_await destroy_smp_service_group(group);
    });
}
