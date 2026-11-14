// Requests that wait for a reply that never comes. Their memory stays
// reachable, so LeakSanitizer doesn't report it, but it keeps growing.
#include <seastar/core/app-template.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/memory.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/core/sleep.hh>
#include <fmt/format.h>
#include <algorithm>
#include <print>
#include <ranges>
#include <unordered_set>
#include <vector>

using namespace seastar;

// snippet: stuck
// Each request waits for its reply, on a condition variable that the code
// that receives replies signals. One reply in a hundred is lost, and its
// request waits forever: a bug, which a timeout would fix.
struct server {
    condition_variable replies;
    std::unordered_set<int> answered;
    gate requests;

    future<> request(int id) {
        auto holder = requests.hold();
        std::vector<char> buffer(1000);     // the request's own memory
        co_await replies.wait([this, id] { return answered.contains(id); });
        answered.erase(id);
    }
    void reply(int id) {
        answered.insert(id);
        replies.broadcast();
    }
    // Wakes the requests still waiting, with an exception, and waits for
    // them to end.
    future<> stop() {
        replies.broken();
        return requests.close();
    }
};
// end snippet: stuck

// snippet: rounds
int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        server s;
        int id = 0;
        for (int round = 1; round <= 5; round++) {
            for (int i = 0; i < 10000; i++, id++) {
                (void)s.request(id).handle_exception([] (auto) {});
                if (id % 100 != 0) {
                    s.reply(id);
                }
            }
            // Let the requests that got their replies end, before measuring.
            co_await sleep(std::chrono::milliseconds(10));
            auto stats = memory::stats();
            std::println("after round {}: {} KB allocated, {} live objects",
                         round, stats.allocated_memory() / 1024,
                         stats.live_objects());
        }
        // snippet: profile
        // With --heapprof, Seastar samples allocations: which places
        // allocated the memory still in use? The three that hold the most.
        auto sites = memory::sampled_memory_profile();
        std::ranges::sort(sites, std::greater{}, &memory::allocation_site::size);
        for (auto& site : sites | std::views::take(3)) {
            fmt::print("{} sampled objects, {} KB, allocated at: {}\n",
                       site.count, site.size / 1024, site.backtrace);
        }
        // end snippet: profile
        co_await s.stop();
    });
}
// end snippet: rounds
