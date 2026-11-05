// An awaitable of our own: co_await sleep_for(d) suspends the coroutine
// for d, with a timer that schedules the coroutine, as a task, when it
// fires. It needs no future and no promise, so it allocates nothing, unlike
// seastar::sleep(). Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/task.hh>
#include <seastar/core/timer.hh>
#include <chrono>
#include <coroutine>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: sleep-for
// An awaitable: co_await sleep_for(d) waits for d. The object lives in the
// coroutine's frame while the coroutine waits, so its timer does too.
class sleep_for {
    std::chrono::steady_clock::duration _how_long;
    timer<> _timer;
public:
    explicit sleep_for(std::chrono::steady_clock::duration how_long)
        : _how_long(how_long) {}
    // No need to suspend for no time at all.
    bool await_ready() const noexcept {
        return _how_long <= std::chrono::steady_clock::duration::zero();
    }
    // The coroutine is suspended: arm a timer that, when it fires, puts
    // the coroutine, which is a task, in the reactor's queue.
    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> h) noexcept {
        _timer.set_callback([&task = h.promise()] {
            schedule(&task);
        });
        _timer.arm(_how_long);
    }
    // The value of the co_await expression: nothing.
    void await_resume() noexcept {}
};
// end snippet: sleep-for

// snippet: compare
// Sleeps a millisecond n times, and returns how many allocations that made.
template <typename Sleep>
future<uint64_t> allocations(int n, Sleep sleep) {
    auto before = memory::stats().mallocs();
    for (int i = 0; i < n; i++) {
        co_await sleep();
    }
    co_return memory::stats().mallocs() - before;
}
// end snippet: compare

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        auto start = std::chrono::steady_clock::now();
        co_await sleep_for(10ms);
        std::chrono::duration<double, std::milli> ms =
            std::chrono::steady_clock::now() - start;
        std::println("sleep_for(10ms) slept {:.1f} ms", ms.count());
        auto ours = co_await allocations(100, [] { return sleep_for(1ms); });
        auto seastars = co_await allocations(100, [] { return sleep(1ms); });
        std::println("100 sleeps: sleep_for() allocated {} times, "
                     "seastar::sleep() {}", ours, seastars);
    });
}
