// What the future of a coroutine looks like when the call returns: a
// coroutine runs until it first waits, and then its caller gets the future.
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
        future<int> a = answer();
        future<int> b = no_answer();
        future<int> c = answer_later();
        describe("answer()", a);
        describe("no_answer()", b);
        describe("answer_later()", c);
        std::println("answer() gives {}", co_await std::move(a));
        try {
            co_await std::move(b);
        } catch (std::runtime_error& e) {
            std::println("no_answer() throws: {}", e.what());
        }
        std::println("answer_later() gives {}", co_await std::move(c));
        // end snippet: main
    });
}
