// A Seastar thread: code that waits for futures by calling get(), on a
// stack of its own, and what a thread costs, against a coroutine.
// Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/thread.hh>
#include <seastar/util/later.hh>
#include <chrono>
#include <print>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

// snippet: wait
// Waits for a future by calling get(): only in a Seastar thread.
int slow_answer() {
    sleep(100ms).get();
    return 42;
}

future<int> answer_in_thread() {
    return async([] {
        auto start = clk::now();
        int answer = slow_answer();
        using ms = std::chrono::duration<double, std::milli>;
        std::println("the answer, {}, took {:.0f} ms", answer,
                     ms(clk::now() - start).count());
        return answer;
    });
}
// end snippet: wait

// snippet: cost
// Starts n threads, or n coroutines, that each yield once, and waits for
// them all; returns how long each took, in nanoseconds.
future<double> each_thread(int n) {
    auto start = clk::now();
    std::vector<future<>> all;
    for (int i = 0; i < n; i++) {
        all.push_back(async([] { thread::yield(); }));
    }
    for (auto& f : all) {
        co_await std::move(f);
    }
    co_return std::chrono::duration<double, std::nano>(clk::now() - start)
              .count() / n;
}

future<double> each_coroutine(int n) {
    auto start = clk::now();
    std::vector<future<>> all;
    for (int i = 0; i < n; i++) {
        all.push_back([] () -> future<> { co_await yield(); }());
    }
    for (auto& f : all) {
        co_await std::move(f);
    }
    co_return std::chrono::duration<double, std::nano>(clk::now() - start)
              .count() / n;
}
// end snippet: cost

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await answer_in_thread();
        // snippet: rounds
        for (int round = 1; round <= 3; round++) {
            auto t = co_await each_thread(1'000);
            auto c = co_await each_coroutine(1'000);
            std::println("round {}: a thread {:.0f} ns, a coroutine {:.0f} ns",
                         round, t, c);
        }
        // end snippet: rounds
    });
}
