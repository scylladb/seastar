// Chapter 23, exercise 1: when-all.cc's add_two() and one_fails(), as
// coroutines. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/when_all.hh>
#include <chrono>
#include <exception>
#include <print>
#include <stdexcept>
#include <tuple>

using namespace seastar;
using namespace std::chrono_literals;

// A number, after a while.
future<int> slow_number(int n, std::chrono::milliseconds delay) {
    return sleep(delay).then([n] {
        return n;
    });
}

// A failure, after a while.
future<int> slow_failure(std::chrono::milliseconds delay) {
    return sleep(delay).then([] () -> int {
        throw std::runtime_error("failed");
    });
}

// snippet: coroutines
future<> add_two() {
    auto [a, b] = co_await when_all_succeed(slow_number(1, 20ms),
                                            slow_number(2, 10ms));
    std::println("when_all_succeed: {} + {} = {}", a, b, a + b);
}

future<> one_fails() {
    auto [number, failure] = co_await when_all(slow_number(3, 10ms),
                                               slow_failure(20ms));
    std::println("when_all: {}, and {}", number.get(),
                 failure.failed() ? "a failure" : "a number");
    failure.ignore_ready_future();
}
// end snippet: coroutines

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        return add_two().then([] {
            return one_fails();
        });
    });
}
