// Generators: Seastar's, which may wait between the values it yields, in
// its unbuffered and buffered variants, and C++23's std::generator, which
// can't wait, but needs no future to consume. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/coroutine/generator.hh>
#include <chrono>
#include <generator>
#include <print>
#include <utility>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: seastar-generator
// Counts down from n, waiting a millisecond before each value: the
// generator is a coroutine, which may co_await between its co_yields.
coroutine::experimental::generator<int> count_down(int n) {
    for (int i = n; i > 0; i--) {
        co_await sleep(1ms);
        co_yield i;
    }
}

future<> print_count_down() {
    auto numbers = count_down(3);
    // co_await numbers() runs the generator to its next co_yield, and
    // returns the value, or nothing at the end.
    while (auto n = co_await numbers()) {
        std::println("count_down: {}", n->get());
    }
}
// end snippet: seastar-generator

// snippet: buffered
// The same idea, buffered: the generator yields a whole batch of values
// at once, with elements_of(), and the consumer takes them one by one,
// without switching to the generator for each.
coroutine::experimental::generator<int, int, std::vector<int>>
squares(int n) {
    std::vector<int> batch;
    for (int i = 1; i <= n; i++) {
        batch.push_back(i * i);
        if (batch.size() == 4 || i == n) {
            co_yield coroutine::experimental::elements_of(
                std::exchange(batch, {}));
        }
    }
}

future<> print_squares() {
    auto numbers = squares(6);
    while (auto n = co_await numbers()) {
        std::println("squares: {}", *n);
    }
}
// end snippet: buffered

// snippet: std-generator
// C++23's std::generator: a synchronous coroutine, consumed with a plain
// for loop. It can't co_await, but a Seastar program can use it for code
// that never waits.
std::generator<int> evens(int n) {
    for (int i = 0; i < n; i += 2) {
        co_yield i;
    }
}
// end snippet: std-generator

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await print_count_down();
        co_await print_squares();
        for (int even : evens(7)) {
            std::println("evens: {}", even);
        }
    });
}
