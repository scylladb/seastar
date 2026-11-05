// A periodic timer, and a one-shot timer that cancels it.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/timer.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: timers
future<> tick_and_stop() {
    int ticks = 0;
    timer<> ticker([&ticks] {
        std::println("tick {}", ++ticks);
    });
    ticker.arm_periodic(10ms);
    timer<> stopper([&ticker] {
        std::println("stopping the ticker; armed: {}", ticker.armed());
        ticker.cancel();
    });
    stopper.arm(35ms);
    co_await sleep(60ms);
    std::println("{} ticks; armed: {}", ticks, ticker.armed());
}
// end snippet: timers

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        return tick_and_stop();
    });
}
