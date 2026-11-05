// Two coroutines that wait at the same time, on one thread.
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
// Counts to n, one number every interval.
future<> count(std::string_view name, int n,
               std::chrono::milliseconds interval) {
    for (int i = 1; i <= n; i++) {
        co_await sleep(interval);
        std::println("{} {}", name, i);
    }
}
// end snippet: count

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: one-after-another
        auto start = std::chrono::steady_clock::now();
        co_await count("tick", 3, 300ms);
        co_await count("tock", 2, 500ms);
        std::println("one after the other: {}",
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start));
        // end snippet: one-after-another

        // snippet: together
        start = std::chrono::steady_clock::now();
        co_await coroutine::all(
            [] { return count("tick", 3, 300ms); },
            [] { return count("tock", 2, 500ms); });
        std::println("together: {}",
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start));
        // end snippet: together
    });
}
