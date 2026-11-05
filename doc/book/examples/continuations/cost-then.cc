// What a continuation costs, against a coroutine, when the future it
// waits for is already available: a function that doubles the result of
// another, called a hundred million times each way. Run it with one
// shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <chrono>
#include <cstdint>
#include <print>
#include <string_view>

using namespace seastar;

// snippet: functions
[[gnu::noinline]] future<int> ready(int x) {
    return make_ready_future<int>(x + 1);
}

// Doubles ready()'s result, with a continuation...
[[gnu::noinline]] future<int> doubled_then(int x) {
    return ready(x).then([] (int y) {
        return y * 2;
    });
}

// ... and in a coroutine.
[[gnu::noinline]] future<int> doubled_coroutine(int x) {
    co_return co_await ready(x) * 2;
}
// end snippet: functions

constexpr int times = 100'000'000;

future<int64_t> call(auto f) {
    int64_t sum = 0;
    for (int i = 0; i < times; i++) {
        sum += co_await f(i);
    }
    co_return sum;
}

template <typename Func>
future<> measure(std::string_view what, Func f) {
    auto start = std::chrono::steady_clock::now();
    int64_t sum = co_await call(f);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::println("{:<30} {:>5.1f} ns per call (sum {})",
                 what, double(ns) / times, sum);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await measure("ready()", ready);
        co_await measure("doubled with then()", doubled_then);
        co_await measure("doubled in a coroutine", doubled_coroutine);
    });
}
