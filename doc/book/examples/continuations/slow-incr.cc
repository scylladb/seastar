// Simple continuations: an increment that takes a while, a function that
// waits twice, and a chain of increments, each waiting for the one before.
// Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: slow-incr
// Returns i + 1, after 100 milliseconds.
future<int> slow_incr(int i) {
    return sleep(100ms).then([i] {
        return i + 1;
    });
}
// end snippet: slow-incr

// snippet: slower-incr
// Returns i + 1, after sleeping twice, for 100 and then 50 milliseconds.
future<int> slower_incr(int i) {
    return sleep(100ms).then([] {
        return sleep(50ms);
    }).then([i] {
        return i + 1;
    });
}
// end snippet: slower-incr

// snippet: add-three
// Returns i + 3, by incrementing it three times, one after the other.
future<int> add_three(int i) {
    return slow_incr(i).then([] (int j) {
        return slow_incr(j);
    }).then([] (int k) {
        return slow_incr(k);
    });
}
// end snippet: add-three

// Milliseconds since start.
static long since(std::chrono::steady_clock::time_point start) {
    auto d = std::chrono::steady_clock::now() - start;
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        auto start = std::chrono::steady_clock::now();
        // snippet: main
        return slow_incr(1).then([start] (int n) {
            std::println("{} ms: slow_incr(1) = {}", since(start), n);
            return slower_incr(1);
        }).then([start] (int n) {
            std::println("{} ms: slower_incr(1) = {}", since(start), n);
            return add_three(1);
        }).then([start] (int n) {
            std::println("{} ms: add_three(1) = {}", since(start), n);
        });
        // end snippet: main
    });
}
