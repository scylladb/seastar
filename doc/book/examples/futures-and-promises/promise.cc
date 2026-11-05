// The other end of a future: a promise. A sleep() of our own, built from a
// timer and a promise, and what happens to a promise that is never kept.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/timer.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: my-sleep
// Like seastar::sleep(): returns a future that becomes available when the
// time is up.
future<> my_sleep(std::chrono::milliseconds duration) {
    promise<> done;
    timer<> t([&done] { done.set_value(); });
    t.arm(duration);
    co_await done.get_future();
}
// end snippet: my-sleep

// snippet: broken
// Returns the future of a promise that it then destroys, unkept.
future<int> broken() {
    promise<int> p;
    return p.get_future();
}
// end snippet: broken

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        auto start = std::chrono::steady_clock::now();
        co_await my_sleep(200ms);
        std::println("slept for {}",
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start));
        // snippet: broken-call
        try {
            co_await broken();
        } catch (broken_promise& e) {
            std::println("broken() fails: {}", e.what());
        }
        // end snippet: broken-call
    });
}
