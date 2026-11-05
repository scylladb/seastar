// Chapter 1, exercise 2: threads that increment one shared counter, a
// counter of their own in a cache line of its own, or counters of their
// own that sit next to each other. Plain C++ threads, no Seastar.
// Usage: case-for-seastar-ex2 VARIANT THREADS, where VARIANT is shared,
// padded, adjacent or plain.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <print>
#include <string>
#include <thread>
#include <vector>

// snippet: counters
constexpr long increments = 100'000'000;

std::atomic<long> shared_counter;

// Each counter in a cache line of its own.
struct alignas(64) padded_counter {
    std::atomic<long> value;
};
padded_counter padded[64];

// Counters next to each other, eight to a 64-byte cache line.
std::atomic<long> adjacent[64];

// Plain variables, next to each other.
long plain[64];
// end snippet: counters

int main(int argc, char** argv) {
    if (argc != 3) {
        std::println(stderr, "usage: {} VARIANT THREADS", argv[0]);
        return 2;
    }
    std::string variant = argv[1];
    int threads = std::atoi(argv[2]);
    auto start = std::chrono::steady_clock::now();
    {
        std::vector<std::jthread> workers;
        for (int t = 0; t < threads; t++) {
            // snippet: work
            workers.emplace_back([&variant, t] {
                for (long i = 0; i < increments; i++) {
                    auto relaxed = std::memory_order_relaxed;
                    if (variant == "shared") {
                        shared_counter.fetch_add(1, relaxed);
                    } else if (variant == "padded") {
                        padded[t].value.fetch_add(1, relaxed);
                    } else if (variant == "adjacent") {
                        adjacent[t].fetch_add(1, relaxed);
                    } else {
                        plain[t]++;
                    }
                }
            });
            // end snippet: work
        }
    }
    std::chrono::duration<double> secs =
        std::chrono::steady_clock::now() - start;
    std::println("{}, {} threads: {:.2f} s, {:.1f} ns per increment",
                 variant, threads, secs.count(),
                 secs.count() * 1e9 / increments);
}
