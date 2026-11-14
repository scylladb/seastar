// Testing code that waits, with manual_clock: time moves only when the test
// moves it.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/util/later.hh>
#include <seastar/core/manual_clock.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: manual
// Code to test: calls its function after a delay, measured with the clock
// it's given.
template <typename Clock>
class delayed {
    timer<Clock> _timer;
public:
    delayed(typename Clock::duration delay, noncopyable_function<void ()> f)
        : _timer(std::move(f)) {
        _timer.arm(delay);
    }
};

future<> test_delayed() {
    bool called = false;
    delayed<manual_clock> d(10s, [&called] { called = true; });
    manual_clock::advance(9s);
    co_await yield();      // let the expired timers, if any, run
    std::println("after 9 s: {}", called);
    manual_clock::advance(1s);
    co_await yield();
    std::println("after 10 s: {}", called);
}
// end snippet: manual

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        return test_delayed();
    });
}
