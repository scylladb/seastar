// The solution of chapter 28's exercise 2: a program that relies on the
// order in which two fibers run, which works in a release build and fails
// in a debug build, and its correct version.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/later.hh>
#include <optional>
#include <print>
#include <string>

using namespace seastar;

// snippet: wrong
// One fiber prepares a greeting, after a yield, which stands for anything
// that makes it wait; another prints it, after its own yield, assuming
// that by then the first fiber is done: it yielded first, so it will
// continue first. print() returns whether the greeting was there.
future<> prepare(std::optional<std::string>& greeting) {
    co_await yield();
    greeting = "hello";
}

future<bool> print(const std::optional<std::string>& greeting) {
    co_await yield();
    co_return greeting.has_value();
}

future<bool> wrong() {
    std::optional<std::string> greeting;
    auto [prepared, printed] = co_await when_all(prepare(greeting),
                                                 print(greeting));
    co_return printed.get();
}
// end snippet: wrong

// snippet: right
// The correct version: the printer waits for the greeting, with a future
// that the preparer makes ready, rather than for its turn.
future<> prepare(std::optional<std::string>& greeting, promise<>& ready) {
    co_await yield();
    greeting = "hello";
    ready.set_value();
}

future<bool> print(const std::optional<std::string>& greeting,
                   future<> ready) {
    co_await std::move(ready);
    co_return greeting.has_value();
}

future<bool> right() {
    std::optional<std::string> greeting;
    promise<> ready;
    auto [prepared, printed] = co_await when_all(
            prepare(greeting, ready), print(greeting, ready.get_future()));
    co_return printed.get();
}
// end snippet: right

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        int wrong_missing = 0, right_missing = 0;
        for (int i = 0; i < 100; i++) {
            wrong_missing += !co_await wrong();
            right_missing += !co_await right();
        }
        std::println("wrong: no greeting in {} of 100", wrong_missing);
        std::println("right: no greeting in {} of 100", right_missing);
    });
}
