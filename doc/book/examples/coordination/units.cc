// A semaphore counts units: of concurrency, or of anything else, such as
// bytes of memory. Waiters are served in the order in which they came.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sleep.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <algorithm>
#include <chrono>
#include <print>
#include <ranges>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: concurrency
// At most three downloads at a time.
semaphore downloads(3);
int running = 0, most_running = 0;

future<> download(int i) {
    auto held = co_await get_units(downloads, 1);
    running++;
    most_running = std::max(most_running, running);
    co_await sleep(10ms);       // the download itself
    running--;
}
// end snippet: concurrency

future<> limit_concurrency() {
    co_await coroutine::parallel_for_each(std::views::iota(0, 10), download);
    std::println("10 downloads, at most {} at a time", most_running);
}

future<> count_bytes() {
    // snippet: bytes
    semaphore memory(100);      // bytes
    auto first = co_await get_units(memory, 60);
    // 40 bytes are left. A request for 50 waits, and so does a request
    // for 10, which would fit, because it came after the request for 50.
    auto second = get_units(memory, 50);
    auto third = get_units(memory, 10);
    std::println("{} left, {} requests waiting", memory.available_units(),
                 memory.waiters());
    first.return_all();
    std::println("{} left, {} requests waiting", memory.available_units(),
                 memory.waiters());
    // end snippet: bytes
    auto units2 = co_await std::move(second);
    auto units3 = co_await std::move(third);
}

future<> too_many() {
    // snippet: too-many
    named_semaphore memory(100, named_semaphore_exception_factory{"memory"});
    try {
        // More units than the semaphore will ever have: without a timeout,
        // this would wait forever.
        auto units = co_await get_units(memory, 200, 100ms);
    } catch (semaphore_timed_out& e) {
        std::println("{}", e.what());
    }
    // end snippet: too-many
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await limit_concurrency();
        co_await count_bytes();
        co_await too_many();
    });
}
