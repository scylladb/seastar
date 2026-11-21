// Waiting for several futures with continuations: when_all_succeed() and
// then_unpack(), and when_all(), which waits for all of them even if some
// fail. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
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

// snippet: unpack
// Waits for two numbers, and adds them.
future<> add_two() {
    return when_all_succeed(slow_number(1, 20ms), slow_number(2, 10ms))
            .then_unpack([] (int a, int b) {
        std::println("when_all_succeed: {} + {} = {}", a, b, a + b);
    });
}
// end snippet: unpack

// snippet: tuple
// Waits for a number and a failure, and looks at both.
future<> one_fails() {
    return when_all(slow_number(3, 10ms), slow_failure(20ms)).then(
            [] (std::tuple<future<int>, future<int>> results) {
        auto& [number, failure] = results;
        std::println("when_all: {}, and {}", number.get(),
                     failure.failed() ? "a failure" : "a number");
        failure.ignore_ready_future();
    });
}
// end snippet: tuple

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        return add_two().then([] {
            return one_fails();
        });
    });
}
