// Chapter 9, exercise 3: timeouts.cc's with_timeout(), with an operation
// that takes a reference to a local variable of its caller.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/with_timeout.hh>
#include <chrono>
#include <print>
#include <string>

using namespace seastar;
using namespace std::chrono_literals;

// An operation that takes 100 milliseconds, and then prints its name.
future<int> slow_operation(std::string& name) {
    co_await sleep(100ms);
    std::println("slow_operation({}) finished", name);
    co_return 42;
}

future<> try_with_timeout() {
    // Long enough that its characters are on the heap.
    std::string name = "the operation that timed out";
    try {
        int n = co_await with_timeout(lowres_clock::now() + 20ms,
                                      slow_operation(name));
        std::println("got {}", n);
    } catch (timed_out_error&) {
        std::println("timed out");
    }
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await try_with_timeout();
        co_await sleep(150ms);
        std::println("done");
    });
}
