// Where stalls come from: ordinary operations, each timed as it runs on a
// shard, which can't run anything else meanwhile. Run it with one shard and
// 4 GB: -c1 -m4G.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/util/later.hh>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <print>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace seastar;
using clk = std::chrono::steady_clock;

// snippet: timed
// Runs f, and prints how long the shard was busy with it.
template <typename Func>
void timed(std::string_view what, Func f) {
    auto start = clk::now();
    f();
    std::chrono::duration<double, std::milli> ms = clk::now() - start;
    std::println("{:<46} {:7.1f} ms", what, ms.count());
}
// end snippet: timed

// Keeps the compiler from optimizing away work whose result nothing reads.
void escape(const void* p) {
    asm volatile("" : : "g"(p) : "memory");
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        std::mt19937 random(1);
        // snippet: sources
        // A million strings, each in an allocation of its own.
        auto strings = std::make_unique<std::vector<std::string>>();
        for (int i = 0; i < 1'000'000; i++) {
            strings->push_back(std::string(40, 'x'));
        }
        co_await yield();
        timed("destroying a vector of a million strings", [&] {
            strings.reset();
        });
        auto map = std::make_unique<std::unordered_map<int, int>>();
        for (int i = 0; i < 1'000'000; i++) {
            (*map)[i] = i;
        }
        co_await yield();
        timed("destroying an unordered_map of a million keys", [&] {
            map.reset();
        });
        std::vector<int> numbers(10'000'000);
        for (auto& n : numbers) {
            n = random();
        }
        co_await yield();
        timed("sorting ten million numbers", [&] {
            std::ranges::sort(numbers);
        });
        timed("copying them (40 MB)", [&] {
            auto copy = numbers;
            escape(copy.data());
        });
        timed("formatting them into a log message", [&] {
            auto message = fmt::format("numbers: {}", numbers);
            escape(message.data());
        });
        timed("throwing and catching 100,000 exceptions", [] {
            for (int i = 0; i < 100'000; i++) {
                try {
                    throw std::runtime_error("failed");
                } catch (std::exception& e) {
                    escape(e.what());
                }
            }
        });
        timed("writing to 512 MB of fresh memory", [] {
            auto memory = std::make_unique_for_overwrite<char[]>(512 << 20);
            std::memset(memory.get(), 1, 512 << 20);
            escape(memory.get());
        });
        timed("writing to the same 512 MB again", [] {
            auto memory = std::make_unique_for_overwrite<char[]>(512 << 20);
            std::memset(memory.get(), 1, 512 << 20);
            escape(memory.get());
        });
        // end snippet: sources
    });
}
