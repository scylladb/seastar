// Three ways for a coroutine's error to reach its caller, and what each
// costs: a thrown exception, an exception returned without throwing, and an
// exception checked without ever being thrown.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/coroutine/try_future.hh>
#include <chrono>
#include <charconv>
#include <exception>
#include <print>
#include <stdexcept>
#include <string_view>

using namespace seastar;

struct parse_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// snippet: throwing
future<int> parse_throwing(std::string_view s) {
    int n;
    auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (err != std::errc() || end != s.data() + s.size()) {
        throw parse_error("not a number");
    }
    co_return n;
}
// end snippet: throwing

// snippet: returning
future<int> parse_returning(std::string_view s) {
    int n;
    auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (err != std::errc() || end != s.data() + s.size()) {
        co_await coroutine::return_exception(parse_error("not a number"));
    }
    co_return n;
}
// end snippet: returning

// snippet: passing
// Parses a number and doubles it, passing a parse error on to the caller.
future<int> parse_and_double(std::string_view s) {
    int n = co_await coroutine::try_future(parse_returning(s));
    co_return 2 * n;
}
// end snippet: passing

// The same, passing the error on by letting co_await throw it.
future<int> parse_and_double_throwing(std::string_view s) {
    int n = co_await parse_returning(s);
    co_return 2 * n;
}

// snippet: catching
// Counts how many of the inputs aren't numbers, catching the exceptions.
future<int> count_errors_catching(auto parse, std::string_view input,
                                  int times) {
    int errors = 0;
    for (int i = 0; i < times; i++) {
        try {
            co_await parse(input);
        } catch (parse_error&) {
            errors++;
        }
    }
    co_return errors;
}
// end snippet: catching

// snippet: checking
// The same, checking each future's result instead of catching.
future<int> count_errors_checking(auto parse, std::string_view input,
                                  int times) {
    int errors = 0;
    for (int i = 0; i < times; i++) {
        auto f = co_await coroutine::as_future(parse(input));
        if (f.failed()) {
            f.ignore_ready_future();
            errors++;
        }
    }
    co_return errors;
}
// end snippet: checking

// The same, counting only parse errors, and checking the type the only way
// that C++23 has: by rethrowing the exception (other errors are rethrown).
future<int> count_parse_errors_rethrowing(auto parse, std::string_view input,
                                          int times) {
    int errors = 0;
    for (int i = 0; i < times; i++) {
        auto f = co_await coroutine::as_future(parse(input));
        if (f.failed()) {
            try {
                std::rethrow_exception(f.get_exception());
            } catch (parse_error&) {
                errors++;
            }
        }
    }
    co_return errors;
}

#if __cpp_lib_exception_ptr_cast
// The same, counting only parse errors, and failing with any other error.
future<int> count_parse_errors_checking(auto parse, std::string_view input,
                                        int times) {
    int errors = 0;
    for (int i = 0; i < times; i++) {
        auto f = co_await coroutine::as_future(parse(input));
        // snippet: checking-type
        if (f.failed()) {
            auto e = f.get_exception();
            if (std::exception_ptr_cast<parse_error>(e)) {
                errors++;
            } else {
                co_await coroutine::return_exception_ptr(std::move(e));
            }
        }
        // end snippet: checking-type
    }
    co_return errors;
}
#endif

template <typename Func>
future<> measure(std::string_view what, Func func) {
    constexpr int times = 1'000'000;
    auto start = std::chrono::steady_clock::now();
    int errors = co_await func(times);
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::println("{:<50} {:>6.0f} ns each ({} errors)",
                 what, double(ns) / times, errors);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await measure("success", [] (int n) {
            return count_errors_checking(parse_returning, "42", n);
        });
        co_await measure("throw, catch", [] (int n) {
            return count_errors_catching(parse_throwing, "x", n);
        });
        co_await measure("return_exception, catch", [] (int n) {
            return count_errors_catching(parse_returning, "x", n);
        });
        co_await measure("return_exception, as_future", [] (int n) {
            return count_errors_checking(parse_returning, "x", n);
        });
        co_await measure("return_exception, co_await, as_future", [] (int n) {
            return count_errors_checking(parse_and_double_throwing, "x", n);
        });
        co_await measure("return_exception, try_future, as_future", [] (int n) {
            return count_errors_checking(parse_and_double, "x", n);
        });
        co_await measure("return_exception, as_future, rethrow", [] (int n) {
            return count_parse_errors_rethrowing(parse_returning, "x", n);
        });
#if __cpp_lib_exception_ptr_cast
        co_await measure("return_exception, as_future, exception_ptr_cast", [] (int n) {
            return count_parse_errors_checking(parse_returning, "x", n);
        });
        // Errors of other types pass through.
        auto other = [] (std::string_view) {
            return make_exception_future<int>(std::logic_error("other"));
        };
        auto f = co_await coroutine::as_future(
            count_parse_errors_checking(other, "x", 1));
        try {
            f.get();
        } catch (std::logic_error&) {
            std::println("other errors pass through");
        }
#endif
    });
}
