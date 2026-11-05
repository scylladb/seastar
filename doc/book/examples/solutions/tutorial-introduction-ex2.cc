// Chapter 2, exercise 2: three counts at the same time, each printing the
// time it ends.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/coroutine/all.hh>
#include <chrono>
#include <print>
#include <string_view>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: count
// Counts to n, one number every interval, and says when it's done.
future<> count(std::string_view name, int n,
               std::chrono::milliseconds interval,
               std::chrono::steady_clock::time_point start) {
    for (int i = 1; i <= n; i++) {
        co_await sleep(interval);
        std::println("{} {}", name, i);
    }
    std::println("{} ends after {} on shard {}", name,
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start),
                 this_shard_id());
}
// end snippet: count

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: together
        auto start = std::chrono::steady_clock::now();
        co_await coroutine::all(
            [start] { return count("tick", 3, 300ms, start); },
            [start] { return count("tock", 2, 500ms, start); },
            [start] { return count("tack", 4, 200ms, start); });
        // end snippet: together
    });
}
