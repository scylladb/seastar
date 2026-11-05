// Chapter 24, exercise 1: awaitable.cc's sleep_for, which an abort source
// can end early, with abort_requested_exception. Run it with one shard:
// -c1.
#include <seastar/core/abort_source.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/task.hh>
#include <seastar/core/timer.hh>
#include <seastar/util/optimized_optional.hh>
#include <chrono>
#include <coroutine>
#include <exception>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

// snippet: abortable
// co_await sleep_for(d, as): waits for d, or until as is triggered, which
// throws abort_requested_exception. The timer and the subscription are
// members of the awaiter, so they live in the coroutine's frame while it
// waits; whichever fires first schedules the coroutine, and cancels the
// other.
class sleep_for {
    clk::duration _how_long;
    abort_source& _as;
    timer<> _timer;
    optimized_optional<abort_source::subscription> _subscription;
    bool _aborted = false;
public:
    sleep_for(clk::duration how_long, abort_source& as)
        : _how_long(how_long), _as(as) {}
    bool await_ready() const noexcept {
        return _as.abort_requested();
    }
    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> h) noexcept {
        task* coroutine = &h.promise();
        _timer.set_callback([this, coroutine] {
            _subscription = {};
            schedule(coroutine);
        });
        _subscription = _as.subscribe([this, coroutine] () noexcept {
            _timer.cancel();
            _aborted = true;
            schedule(coroutine);
        });
        _timer.arm(_how_long);
    }
    void await_resume() {
        if (_aborted || _as.abort_requested()) {
            throw abort_requested_exception();
        }
    }
};
// end snippet: abortable

// Sleeps for how_long, and reports how long it took, and how it ended.
future<> try_sleep(clk::duration how_long, abort_source& as) {
    auto start = clk::now();
    try {
        co_await sleep_for(how_long, as);
        std::print("slept");
    } catch (abort_requested_exception&) {
        std::print("aborted");
    }
    std::chrono::duration<double, std::milli> ms = clk::now() - start;
    std::println(" after {:.0f} ms", ms.count());
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        abort_source as;
        co_await try_sleep(10ms, as);
        // A second sleep, of a second, aborted after 5 ms.
        timer<> abort_later([&as] { as.request_abort(); });
        abort_later.arm(5ms);
        co_await try_sleep(1s, as);
    });
}
