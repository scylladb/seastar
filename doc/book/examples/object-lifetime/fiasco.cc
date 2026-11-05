// The Lambda Coroutine Fiasco: a lambda coroutine given to then() loses its
// captures when it first waits. "Deducing this" moves them into the frame.
// Run with --variant wrong, right or old.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>
#include <string>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: wrong
// Wrong: then() destroys the lambda, and name with it, as soon as the
// coroutine first suspends.
future<> greet_later(std::string name) {
    return sleep(10ms).then([name] () -> future<> {
        co_await sleep(10ms);
        std::println("hello, {}", name);
    });
}
// end snippet: wrong

// snippet: right
// Right: with "this auto", the lambda is moved into the coroutine's frame,
// so name lives as long as the coroutine.
future<> greet_later_right(std::string name) {
    return sleep(10ms).then([name] (this auto) -> future<> {
        co_await sleep(10ms);
        std::println("hello, {}", name);
    });
}
// end snippet: right

// snippet: old
// The workaround in code older than C++23: coroutine::lambda calls the
// lambda without moving it, and the lambda stays a temporary of the
// statement, which waits until the coroutine ends.
future<> greet_later_old(std::string name) {
    co_await sleep(10ms).then(coroutine::lambda([name] () -> future<> {
        co_await sleep(10ms);
        std::println("hello, {}", name);
    }));
}
// end snippet: old

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
        ("variant",
         boost::program_options::value<std::string>()->default_value("right"),
         "wrong, right or old");
    return app.run(argc, argv, [&app] {
        auto variant = app.configuration()["variant"].as<std::string>();
        auto name = std::string("the reader of this book");
        if (variant == "wrong") {
            return greet_later(name);
        } else if (variant == "old") {
            return greet_later_old(name);
        }
        return greet_later_right(name);
    });
}
