// A coroutine that waits for units that it holds itself waits forever, and
// nothing reports it: the shard keeps running everything else.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: deadlock
semaphore table_lock(1);

// Adds an entry to the table's index. The index belongs to the table, so
// this takes the table's lock.
future<> update_index() {
    auto held = co_await get_units(table_lock, 1, 200ms);
    // ...
}

// Adds an entry to the table, and to its index.
future<> update_table() {
    auto held = co_await get_units(table_lock, 1);
    // ...
    co_await update_index();    // waits for the unit that we hold
}
// end snippet: deadlock

// Beats every 10 milliseconds, until told to stop.
future<> heartbeat(const bool& done, int& beats) {
    while (!done) {
        co_await sleep(10ms);
        beats++;
    }
}

future<> run() {
    bool done = false;
    int beats = 0;
    auto beating = heartbeat(done, beats);
    try {
        co_await update_table();
    } catch (semaphore_timed_out& e) {
        std::println("update_index: {}", e.what());
    }
    done = true;
    co_await std::move(beating);
    std::println("meanwhile, the heartbeat beat {} times", beats);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, run);
}
