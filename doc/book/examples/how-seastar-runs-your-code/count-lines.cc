// A long loop that gives up the CPU when its task quota is used up, and
// another task that counts how often it gets to run in the meantime.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/util/later.hh>
#include <cstddef>
#include <print>
#include <string>
#include <utility>
#include <vector>

using namespace seastar;

// snippet: count-lines
// Counts the lines that contain word. The loop has nothing to co_await, so
// it calls coroutine::maybe_yield() to give up the CPU when its quota is
// used up.
future<size_t> count_lines_containing(std::vector<std::string> lines,
                                      std::string word) {
    size_t count = 0;
    for (const auto& line : lines) {
        if (line.contains(word)) {
            count++;
        }
        co_await coroutine::maybe_yield();
    }
    co_return count;
}
// end snippet: count-lines

// Makes the lines "line 0", "line 1", ..., which also takes long enough to
// need maybe_yield().
future<std::vector<std::string>> make_lines(size_t n) {
    std::vector<std::string> lines;
    lines.reserve(n);
    for (size_t i = 0; i < n; i++) {
        lines.push_back("line " + std::to_string(i));
        co_await coroutine::maybe_yield();
    }
    co_return lines;
}

// Counts how many times it gets to run, until told to stop.
future<> other_task(const bool& stop, int& runs) {
    while (!stop) {
        runs++;
        co_await yield();
    }
}

future<> count_lines() {
    auto lines = co_await make_lines(2'000'000);
    bool stop = false;
    int runs = 0;
    auto other = other_task(stop, runs);
    runs = 0;
    auto count = co_await count_lines_containing(std::move(lines), "42");
    stop = true;
    co_await std::move(other);
    std::println("{} lines contain \"42\"; meanwhile, another task ran {} times",
                 count, runs);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        return count_lines();
    });
}
