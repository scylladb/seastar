// Chapter 6, exercise 3: an event, which wakes up every coroutine that is
// waiting for it when it's signaled, built on a shared_promise.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>
#include <utility>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

class event {
    shared_promise<> _p;
public:
    // Waits for the next signal().
    future<> wait() {
        return _p.get_shared_future();
    }
    // Wakes up every coroutine that waits now. Those that call wait()
    // later get the new promise, and wait for the next signal().
    void signal() {
        std::exchange(_p, shared_promise<>()).set_value();
    }
};

future<> waiter(event& e, int who) {
    co_await e.wait();
    std::println("waiter {} woke up", who);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        event e;
        std::vector<future<>> waiting;
        waiting.push_back(waiter(e, 1));
        waiting.push_back(waiter(e, 2));
        co_await sleep(10ms);
        std::println("first signal");
        e.signal();
        waiting.push_back(waiter(e, 3));
        co_await sleep(10ms);
        std::println("second signal");
        e.signal();
        for (auto& f : waiting) {
            co_await std::move(f);
        }
    });
}
