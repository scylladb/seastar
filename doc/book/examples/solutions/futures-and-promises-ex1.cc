// Chapter 6, exercise 1: states.cc, calling get() on a future that isn't
// available yet.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>
#include <stdexcept>
#include <string_view>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: coroutines
// Returns its result without waiting.
future<int> answer() {
    co_return 42;
}

// Fails without waiting.
future<int> no_answer() {
    throw std::runtime_error("no answer");
    co_return 0;    // never reached, but makes this function a coroutine
}

// Waits before returning its result.
future<int> answer_later() {
    std::println("answer_later() starts");
    co_await sleep(10ms);
    std::println("answer_later() resumes");
    co_return 42;
}
// end snippet: coroutines

// snippet: describe
void describe(std::string_view name, const future<int>& f) {
    if (!f.available()) {
        std::println("{}: not available", name);
    } else if (f.failed()) {
        std::println("{}: failed", name);
    } else {
        std::println("{}: available", name);
    }
}
// end snippet: describe

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: main
        future<int> f1 = answer();
        future<int> f2 = no_answer();
        future<int> f3 = answer_later();
        describe("answer()", f1);
        describe("no_answer()", f2);
        describe("answer_later()", f3);
        // Exercise 1: ask for answer_later()'s result before it's available.
        std::println("answer_later() gives {}, without waiting", f3.get());
        std::println("answer() gives {}", co_await std::move(f1));
        try {
            co_await std::move(f2);
        } catch (std::runtime_error& e) {
            std::println("no_answer() throws: {}", e.what());
        }
        std::println("answer_later() gives {}", co_await std::move(f3));
        // end snippet: main
    });
}
