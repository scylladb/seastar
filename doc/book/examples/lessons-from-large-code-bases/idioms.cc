// Three small utilities that large Seastar programs write for themselves:
// a retry with exponential backoff, an action whose triggers coalesce, and
// a barrier that waits for the operations of earlier phases.
#include <seastar/core/abort_source.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/noncopyable_function.hh>
#include <algorithm>
#include <chrono>
#include <print>
#include <random>
#include <stdexcept>
#include <utility>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: retry
// Calls f() until it succeeds, at most `attempts` times. Between tries it
// waits, a delay that starts at `first` and doubles each time, up to
// `max`, and of which a random part, so that clients that failed together
// don't retry together. An abort of `as` ends the wait at once, with
// sleep_aborted; the last failure is rethrown.
template <typename Func>
future<> retry_with_backoff(Func f, int attempts, std::chrono::milliseconds first,
                            std::chrono::milliseconds max, abort_source& as) {
    static thread_local std::default_random_engine random{std::random_device{}()};
    auto delay = first;
    for (int attempt = 1; ; attempt++) {
        std::exception_ptr failure;
        try {
            co_await f();
            co_return;
        } catch (...) {
            failure = std::current_exception();
        }
        if (attempt == attempts) {
            std::rethrow_exception(failure);
        }
        // Somewhere between half the delay and all of it.
        std::uniform_int_distribution<long> jitter(delay.count() / 2, delay.count());
        co_await sleep_abortable(std::chrono::milliseconds(jitter(random)), as);
        delay = std::min(delay * 2, max);
    }
}
// end snippet: retry

// snippet: coalescing
// Runs an action, one run at a time. trigger() makes sure that a run of
// the action starts after the call, and returns a future that becomes
// ready when that run ends: triggers that come while the action runs all
// wait for the same next run, so many triggers cost few runs. stop()
// waits for the run in progress, and those already promised.
class coalescing_action {
    noncopyable_function<future<>()> _action;
    lw_shared_ptr<shared_promise<>> _next;   // the waiters for the next run
    future<> _runner = make_ready_future<>();
public:
    explicit coalescing_action(noncopyable_function<future<>()> action)
        : _action(std::move(action)) {}

    future<> trigger() {
        if (_next) {
            return _next->get_shared_future();
        }
        _next = make_lw_shared<shared_promise<>>();
        // Take the future first: run() begins at once, and takes _next.
        auto ready = _next->get_shared_future();
        if (_runner.available()) {
            _runner = run();
        }
        return ready;
    }

    future<> stop() {
        return std::exchange(_runner, make_ready_future<>());
    }

private:
    future<> run() {
        while (_next) {
            auto waiters = std::exchange(_next, nullptr);
            try {
                co_await _action();
                waiters->set_value();
            } catch (...) {
                waiters->set_exception(std::current_exception());
            }
        }
    }
};
// end snippet: coalescing

// snippet: barrier
// Tracks operations, and waits for those that began before a point in
// time: start() begins an operation, which lasts as long as the holder
// that it returns, and advance_and_await() begins a new phase, and waits
// for the operations of the earlier ones, but not for those that begin
// after it.
class phased_barrier {
    lw_shared_ptr<gate> _gate = make_lw_shared<gate>();
public:
    gate::holder start() {
        return _gate->hold();
    }

    future<> advance_and_await() {
        auto earlier = std::exchange(_gate, make_lw_shared<gate>());
        co_await earlier->close();
    }
};
// end snippet: barrier

future<> demo_retry() {
    abort_source as;
    int calls = 0;
    co_await retry_with_backoff([&calls] () -> future<> {
        calls++;
        if (calls < 4) {
            std::println("retry: attempt {} failed", calls);
            throw std::runtime_error("not yet");
        }
        std::println("retry: attempt {} succeeded", calls);
        co_return;
    }, 10, 10ms, 100ms, as);
}

future<> demo_coalescing() {
    int runs = 0;
    coalescing_action action([&runs] () -> future<> {
        runs++;
        co_await sleep(10ms);
    });
    // Ten triggers at once: the first starts a run, and the other nine all
    // wait for the one after it.
    std::vector<future<>> triggered;
    for (int i = 0; i < 10; i++) {
        triggered.push_back(action.trigger());
    }
    for (auto& f : triggered) {
        co_await std::move(f);
    }
    co_await action.stop();
    std::println("coalescing: 10 triggers, {} runs", runs);
}

future<> demo_barrier() {
    phased_barrier barrier;
    std::vector<std::string> log;
    auto operation = [&] (std::string name, std::chrono::milliseconds duration) -> future<> {
        auto holder = barrier.start();
        co_await sleep(duration);
        log.push_back(name);
    };
    auto a = operation("a ended", 20ms);
    co_await sleep(1ms);
    auto waited = barrier.advance_and_await().then([&log] {
        log.push_back("barrier passed");
    });
    // b begins after the barrier, and takes longer than a: the barrier
    // doesn't wait for it.
    auto b = operation("b ended", 40ms);
    co_await std::move(a);
    co_await std::move(waited);
    co_await std::move(b);
    std::println("barrier: {}", log);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await demo_retry();
        co_await demo_coalescing();
        co_await demo_barrier();
    });
}
