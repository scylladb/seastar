// Calls between shards that make nested calls back, in smp_service_groups
// that limit how many calls may be in progress: one group for both
// levels deadlocks, a group for each level doesn't. Run with two shards.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/util/log.hh>
#include <fmt/format.h>
#include <chrono>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: nested
// Asks the other shard to work for a moment, and then to ask this shard,
// in turn, to do nothing. outer is the group of the first call, inner of
// the nested call; a nested call that waits a second for its turn fails.
future<> call_and_call_back(smp_service_group outer,
                            smp_service_group inner) {
    unsigned me = this_shard_id();
    unsigned other = 1 - me;
    co_await smp::submit_to(other, outer,
            [me, inner] (this auto) -> future<> {
        co_await sleep(10ms);
        auto deadline = lowres_clock::now() + 1s;
        co_await smp::submit_to(me, smp_submit_to_options(inner, deadline),
                                [] {});
    });
}
// end snippet: nested

// Makes both shards call each other at once, and tells how it went.
future<> both_at_once(const char* name, smp_service_group outer,
                      smp_service_group inner) {
    auto f = co_await coroutine::as_future(smp::invoke_on_all(
        [outer, inner] { return call_and_call_back(outer, inner); }));
    if (f.failed()) {
        fmt::print("{}: {}\n", name, formattable(f.get_exception()));
    } else {
        fmt::print("{}: done\n", name);
    }
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        if (this_smp_shard_count() != 2) {
            throw std::runtime_error("run with two shards (-c2)");
        }
        // snippet: groups
        // Groups that allow one call at a time from a shard to another.
        smp_service_group_config one_at_a_time;
        one_at_a_time.max_nonlocal_requests = 1;
        auto a = co_await create_smp_service_group(one_at_a_time);
        auto b = co_await create_smp_service_group(one_at_a_time);
        co_await both_at_once("one group", a, a);
        co_await both_at_once("two groups", a, b);
        // end snippet: groups
        co_await destroy_smp_service_group(a);
        co_await destroy_smp_service_group(b);
    });
}
