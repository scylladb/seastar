// Exercise 1 of "Lessons from large Seastar code bases": a retry with
// backoff that gives up at a deadline, rather than after a number of
// attempts.
#include <seastar/core/abort_source.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <algorithm>
#include <chrono>
#include <print>
#include <random>
#include <stdexcept>

using namespace seastar;
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

// Calls f() until it succeeds. Between tries it waits, a delay that starts
// at `first` and doubles each time, up to `max`, and of which a random
// part. It gives up, rethrowing the last failure, when the next wait would
// end at or after `deadline`: sleeping only to fail would waste the time.
template <typename Func>
future<> retry_with_backoff(Func f, clock_type::time_point deadline,
                            std::chrono::milliseconds first,
                            std::chrono::milliseconds max, abort_source& as) {
    static thread_local std::default_random_engine random{std::random_device{}()};
    auto delay = first;
    for (;;) {
        std::exception_ptr failure;
        try {
            co_await f();
            co_return;
        } catch (...) {
            failure = std::current_exception();
        }
        std::uniform_int_distribution<long> jitter(delay.count() / 2, delay.count());
        auto wait = std::chrono::milliseconds(jitter(random));
        if (clock_type::now() + wait >= deadline) {
            std::rethrow_exception(failure);
        }
        co_await sleep_abortable(wait, as);
        delay = std::min(delay * 2, max);
    }
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        abort_source as;
        int calls = 0;
        // Fails twice, then succeeds.
        co_await retry_with_backoff([&calls] () -> future<> {
            if (++calls < 3) {
                throw std::runtime_error("not yet");
            }
            co_return;
        }, clock_type::now() + 1s, 10ms, 100ms, as);
        std::println("succeeded at attempt {}", calls);

        // Always fails: gives up before the deadline, 200 ms away.
        calls = 0;
        auto start = clock_type::now();
        try {
            co_await retry_with_backoff([&calls] () -> future<> {
                calls++;
                throw std::runtime_error("never");
            }, start + 200ms, 10ms, 100ms, as);
        } catch (const std::runtime_error& e) {
            auto ms = std::chrono::duration<double, std::milli>(clock_type::now() - start).count();
            std::println("gave up after {} attempts, {:.0f} ms: {}", calls, ms, e.what());
        }
    });
}
