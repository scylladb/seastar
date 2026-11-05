// Chapter 23, exercise 2: errors-then.cc with a parse() that never throws,
// and returns every failure as a failed future. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
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
// Waits a millisecond, and parses a number. An empty string is rejected
// at once, with a failed future, rather than by throwing.
future<int> parse(sstring s) {
    if (s.empty()) {
        return make_exception_future<int>(std::invalid_argument("empty"));
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
        co_await show("21");
        co_await show("abc");
        co_await show("");
    });
}
