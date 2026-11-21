// Data for each scheduling group: a scheduling_group_key gives every
// group, on every shard, a value of its own, which code reaches through
// the current group, without being told which group it runs in. Here, the
// value counts the requests that each group handled.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/scheduling_specific.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <cstdint>
#include <optional>
#include <print>

using namespace seastar;

// snippet: stats
struct request_stats {
    uint64_t requests = 0;
};

// The key that finds each group's request_stats, made when the program
// starts. A key can't be made empty, or assigned, hence the optional.
std::optional<scheduling_group_key> stats_key;
// end snippet: stats

// snippet: handle
// Handles a request, wherever it's called from, and counts it in the
// stats of the group that runs it.
void handle_request() {
    scheduling_group_get_specific<request_stats>(*stats_key).requests++;
}
// end snippet: handle

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: create
        // Every group, existing or made later, gets a request_stats, made
        // by its default constructor.
        stats_key.emplace(co_await scheduling_group_key_create(
            make_scheduling_group_key_config<request_stats>()));
        // end snippet: create
        auto interactive =
            co_await create_scheduling_group("interactive", 1000);
        auto batch = co_await create_scheduling_group("batch", 100);
        for (int i = 0; i < 3; i++) {
            co_await with_scheduling_group(interactive, handle_request);
        }
        for (int i = 0; i < 2; i++) {
            co_await with_scheduling_group(batch, handle_request);
        }
        for (auto sg : {interactive, batch}) {
            auto& stats = sg.get_specific<request_stats>(*stats_key);
            std::println("{}: {} requests", sg.name(), stats.requests);
        }
    });
}
