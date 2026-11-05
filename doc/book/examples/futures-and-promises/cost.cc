// What it costs to call an asynchronous function: one that returns an
// available future without being a coroutine, a coroutine that doesn't
// wait, and a coroutine that waits once; an ordinary function for
// comparison. The functions are kept out of line, as they would be if they
// were in another source file.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/util/later.hh>
#include <chrono>
#include <cstdint>
#include <print>
#include <string_view>

using namespace seastar;

// snippet: functions
[[gnu::noinline]] int plain(int x) {
    return x + 1;
}

[[gnu::noinline]] future<int> ready(int x) {
    return make_ready_future<int>(x + 1);
}

[[gnu::noinline]] future<int> coroutine_ready(int x) {
    co_return x + 1;
}

[[gnu::noinline]] future<int> coroutine_waits(int x) {
    co_await yield();
    co_return x + 1;
}
// end snippet: functions

constexpr int times = 100'000'000;

template <typename Loop>
future<> measure(std::string_view what, Loop loop) {
    auto start = std::chrono::steady_clock::now();
    int64_t sum = co_await loop();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::println("{:<35} {:>5.1f} ns per call (sum {})",
                 what, double(ns) / times, sum);
}

// snippet: loop
future<int64_t> call(auto f) {
    int64_t sum = 0;
    for (int i = 0; i < times; i++) {
        sum += co_await f(i);
    }
    co_return sum;
}
// end snippet: loop

future<int64_t> call_plain() {
    int64_t sum = 0;
    for (int i = 0; i < times; i++) {
        sum += plain(i);
    }
    co_return sum;
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await measure("ordinary function", call_plain);
        co_await measure("make_ready_future()",
                         [] { return call(ready); });
        co_await measure("coroutine that doesn't wait",
                         [] { return call(coroutine_ready); });
        co_await measure("coroutine that waits once",
                         [] { return call(coroutine_waits); });
    });
}
