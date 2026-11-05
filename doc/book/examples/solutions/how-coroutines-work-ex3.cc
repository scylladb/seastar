// Chapter 24, exercise 3: what a value from a generator costs, unbuffered
// and buffered, for ten million values. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/generator.hh>
#include <chrono>
#include <cstdint>
#include <print>
#include <utility>
#include <vector>

using namespace seastar;
using clk = std::chrono::steady_clock;

constexpr int n = 10'000'000;

// snippet: generators
coroutine::experimental::generator<int> unbuffered() {
    for (int i = 0; i < n; i++) {
        co_yield i;
    }
}

// Yields batches of 256 values.
coroutine::experimental::generator<int, int, std::vector<int>> buffered() {
    std::vector<int> batch;
    for (int i = 0; i < n; i++) {
        batch.push_back(i);
        if (batch.size() == 256 || i == n - 1) {
            co_yield coroutine::experimental::elements_of(
                std::exchange(batch, {}));
        }
    }
}
// end snippet: generators

template <typename Generator>
future<> measure(const char* what, Generator values) {
    auto start = clk::now();
    int64_t sum = 0;
    while (auto v = co_await values()) {
        sum += *v;
    }
    std::chrono::duration<double, std::nano> ns = clk::now() - start;
    std::println("{}: {:.1f} ns per value (sum {})", what,
                 ns.count() / n, sum);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await measure("unbuffered", unbuffered());
        co_await measure("buffered", buffered());
    });
}
