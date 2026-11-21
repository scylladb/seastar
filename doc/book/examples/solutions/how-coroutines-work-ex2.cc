// Chapter 24, exercise 2: the frames of a coroutine that awaits another
// coroutine, and of one with a large local variable that doesn't live
// across a suspension. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/memory.hh>
#include <array>
#include <cstddef>
#include <numeric>
#include <print>

using namespace seastar;

// snippet: frames
future<int> inner(future<> wait) {
    co_await std::move(wait);
    co_return 1;
}

// Awaits inner(), which runs inside outer() until it first waits.
future<int> outer(future<> wait) {
    co_return co_await inner(std::move(wait));
}

// A large local variable, used only before the first suspension.
future<int> local_before(int n, future<> wait) {
    std::array<int, 250> numbers;           // 1,000 bytes
    for (int i = 0; i < 250; i++) {
        numbers[i] = n + i;
    }
    int sum = std::accumulate(numbers.begin(), numbers.end(), 0);
    co_await std::move(wait);
    co_return sum;
}
// end snippet: frames

// The allocations, and their bytes, that a call to f makes until it first
// waits.
template <typename Func>
future<> measure(const char* what, Func f) {
    promise<> p;
    auto stats = memory::stats();
    auto fut = f(p.get_future());
    auto after = memory::stats();
    p.set_value();
    co_await std::move(fut);
    auto bytes = after.total_bytes_allocated()
                 - stats.total_bytes_allocated();
    std::println("{}: {} allocations, {} bytes", what,
                 after.mallocs() - stats.mallocs(), bytes);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await measure("inner()", inner);
        co_await measure("outer(), awaiting inner()", outer);
        co_await measure("local_before()", [] (future<> wait) {
            return local_before(1, std::move(wait));
        });
    });
}
