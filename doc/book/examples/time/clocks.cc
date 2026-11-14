// What reading each clock costs, and how lowres_clock stands still while a
// task runs without yielding.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/timer.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

template <typename Clock>
void measure(const char* name) {
    constexpr int n = 10'000'000;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < n; i++) {
        auto t = Clock::now();
        // Use t, and keep the compiler from reading the clock only once.
        asm volatile("" : : "r"(t.time_since_epoch().count()) : "memory");
    }
    std::chrono::duration<double, std::nano> took = std::chrono::steady_clock::now() - start;
    std::println("{:<28} {:5.1f} ns", name, took.count() / n);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        measure<std::chrono::steady_clock>("std::chrono::steady_clock");
        measure<std::chrono::system_clock>("std::chrono::system_clock");
        measure<lowres_clock>("lowres_clock");
        measure<lowres_system_clock>("lowres_system_clock");
        // snippet: stale
        auto lowres_start = lowres_clock::now();
        auto steady_start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - steady_start < 5ms) {
            // busy, without yielding
        }
        auto moved = lowres_clock::now() - lowres_start;
        std::println("in 5 ms of work, lowres_clock moved {} ms",
            std::chrono::duration_cast<std::chrono::milliseconds>(moved).count());
        // end snippet: stale
        co_return;
    });
}
