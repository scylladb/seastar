// How a coroutine runs: at once, inside its caller, until it first
// suspends; and how large its frame is, which is allocated from the shard's
// memory on every call. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/memory.hh>
#include <seastar/util/later.hh>
#include <array>
#include <cstddef>
#include <numeric>
#include <print>

using namespace seastar;

// snippet: order
future<> callee() {
    std::println("  callee: starts, inside its caller");
    co_await yield();
    std::println("  callee: resumes, as a task of its own");
}

future<> caller() {
    std::println("caller: calls the callee");
    auto f = callee();
    std::println("caller: got its future, available: {}", f.available());
    co_await std::move(f);
    std::println("caller: the callee is done");
}
// end snippet: order

// snippet: frames
// Coroutines whose frames hold different local variables. Each waits for
// a future that it's given, which allocates nothing.
future<int> small(int n, future<> wait) {
    int x = n;
    co_await std::move(wait);
    co_return x;
}

future<int> large(int n, future<> wait) {
    std::array<int, 250> numbers;           // 1,000 bytes
    for (int i = 0; i < 250; i++) {
        numbers[i] = n + i;
    }
    co_await std::move(wait);
    co_return std::accumulate(numbers.begin(), numbers.end(), 0);
}

// The memory that a call to f allocates until it first waits: its frame.
template <typename Func>
future<size_t> frame_size(Func f) {
    promise<> p;
    auto before = memory::stats().total_bytes_allocated();
    auto fut = f(1, p.get_future());
    auto after = memory::stats().total_bytes_allocated();
    p.set_value();
    co_await std::move(fut);
    co_return after - before;
}
// end snippet: frames

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await caller();
        std::println("frame of small(): {} bytes",
                     co_await frame_size(small));
        std::println("frame of large(): {} bytes",
                     co_await frame_size(large));
    });
}
