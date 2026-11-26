// A lock for readers and writers: readers may hold it together, a writer
// alone. A reader that comes after a waiting writer waits for it too, so
// that readers can't keep a writer waiting forever.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/rwlock.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/when_all.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: rwlock
rwlock table_lock;

future<> reader(int i) {
    auto held = co_await table_lock.hold_read_lock();
    std::println("reader {} reads", i);
    co_await sleep(10ms);
}

future<> writer() {
    auto held = co_await table_lock.hold_write_lock();
    std::println("the writer writes");
    co_await sleep(10ms);
}

future<> run() {
    co_await when_all(reader(1), reader(2), writer(), reader(3));
}
// end snippet: rwlock

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, run);
}
