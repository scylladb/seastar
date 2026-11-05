// Background work that waits: how long a shutdown waits for it, when the
// wait can be aborted, and when it can't.
#include <seastar/core/abort_source.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

void save_snapshot() {
    std::println("saved a snapshot");
}

// snippet: slow
// Saves a snapshot every second, until stopping is requested.
future<> save_periodically_slow(abort_source& stopping, gate::holder) {
    while (true) {
        co_await sleep(1s);
        if (stopping.abort_requested()) {
            co_return;
        }
        save_snapshot();
    }
}
// end snippet: slow

// snippet: abortable
// The same, but a request to stop ends the wait.
future<> save_periodically(abort_source& stopping, gate::holder) {
    while (true) {
        try {
            co_await sleep_abortable(1s, stopping);
        } catch (sleep_aborted&) {
            co_return;
        }
        save_snapshot();
    }
}
// end snippet: abortable

// snippet: condition
// Saves a snapshot whenever there are changes, until stopping is requested.
// A condition variable's wait doesn't take an abort source, so a
// subscription wakes the wait instead.
future<> save_changes(abort_source& stopping, gate::holder,
                      condition_variable& changed) {
    auto wake = stopping.subscribe([&changed] () noexcept {
        changed.broadcast();
    });
    while (!stopping.abort_requested()) {
        co_await changed.wait();
        if (!stopping.abort_requested()) {
            save_snapshot();
        }
    }
}
// end snippet: condition

// Starts the work in the background, and a tenth of a second later, stops
// it, and measures how long the stop took.
// snippet: stop
template <typename Work>
future<> stop_after_a_while(const char* name, Work work) {
    abort_source stopping;
    gate background;
    (void)work(stopping, background.hold());
    co_await sleep(100ms);
    auto start = std::chrono::steady_clock::now();
    stopping.request_abort();
    co_await background.close();
    std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
    std::println("{}: stopping took {:.1f} seconds", name, took.count());
}
// end snippet: stop

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await stop_after_a_while("sleep()", save_periodically_slow);
        co_await stop_after_a_while("sleep_abortable()", save_periodically);
        condition_variable changed;
        co_await stop_after_a_while("condition_variable",
                [&changed] (abort_source& stopping, gate::holder h) {
            return save_changes(stopping, std::move(h), changed);
        });
    });
}
