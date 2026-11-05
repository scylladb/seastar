// Chapter 4, exercise 5: parse_returning(), the chapter's coroutine,
// against an ordinary function that returns a ready or a failed future,
// on success and on failure.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/coroutine/exception.hh>
#include <charconv>
#include <chrono>
#include <print>
#include <stdexcept>
#include <string_view>

using namespace seastar;

struct parse_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The chapter's coroutine.
future<int> parse_returning(std::string_view s) {
    int n;
    auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (err != std::errc() || end != s.data() + s.size()) {
        co_await coroutine::return_exception(parse_error("not a number"));
    }
    co_return n;
}

// The same, as an ordinary function, which returns a future that is
// already available or already failed.
future<int> parse_ready(std::string_view s) {
    int n;
    auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (err != std::errc() || end != s.data() + s.size()) {
        return make_exception_future<int>(parse_error("not a number"));
    }
    return make_ready_future<int>(n);
}

// Calls parse a million times, checking each result without throwing, as
// the chapter's count_errors_checking() does, and prints what a call
// costs.
future<> measure(std::string_view what, auto parse, std::string_view input) {
    constexpr int times = 1'000'000;
    int errors = 0;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < times; i++) {
        auto f = co_await coroutine::as_future(parse(input));
        if (f.failed()) {
            f.ignore_ready_future();
            errors++;
        }
    }
    std::chrono::duration<double, std::nano> ns =
        std::chrono::steady_clock::now() - start;
    std::println("{:<28} {:>5.0f} ns each ({} errors)",
                 what, ns.count() / times, errors);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await measure("coroutine, success", parse_returning, "42");
        co_await measure("ordinary function, success", parse_ready, "42");
        co_await measure("coroutine, failure", parse_returning, "x");
        co_await measure("ordinary function, failure", parse_ready, "x");
    });
}
