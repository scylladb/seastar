// Errors in continuations: a failed future skips then(), reaches
// handle_exception() and finally(), and then_wrapped() sees both outcomes;
// and the pitfall of a function that throws before it returns its future.
// Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/sstring.hh>
#include <chrono>
#include <exception>
#include <print>
#include <stdexcept>
#include <string>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: parse
// Waits a millisecond, and parses a number. std::stoi() throws
// std::invalid_argument if s isn't one. But an empty string is rejected
// at once, by throwing, before any future is returned.
future<int> parse(sstring s) {
    if (s.empty()) {
        throw std::invalid_argument("empty");
    }
    return sleep(1ms).then([s] {
        return std::stoi(s);
    });
}
// end snippet: parse

// snippet: chain
// Parses s, doubles the number, and prints it, or the error.
future<> show(sstring s) {
    return parse(s).then([] (int n) {
        return n * 2;                       // skipped if parse() failed
    }).then([s] (int doubled) {
        std::println("\"{}\": {}", s, doubled);
    }).handle_exception_type([s] (std::invalid_argument& e) {
        std::println("\"{}\": not a number ({})", s, e.what());
    }).finally([s] {
        std::println("\"{}\": done", s);
    });
}
// end snippet: chain

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: calls
        co_await show("21");
        co_await show("abc");
        try {
            co_await show("");
        } catch (std::exception& e) {
            std::println("show(\"\") threw: {}", e.what());
        }
        // futurize_invoke() turns the exception into a failed future, so
        // the chain after it sees it.
        co_await futurize_invoke(parse, "").then_wrapped([] (future<int> f) {
            try {
                f.get();
            } catch (std::exception& e) {
                std::println("futurize_invoke: a failed future: {}", e.what());
            }
        });
        // end snippet: calls
    });
}
