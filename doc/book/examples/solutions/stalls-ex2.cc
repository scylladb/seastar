// Chapter 22, exercise 2: destroying an unordered_map of a million keys,
// inserted in order, and in a random order. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <algorithm>
#include <chrono>
#include <memory>
#include <numeric>
#include <print>
#include <random>
#include <unordered_map>
#include <vector>

using namespace seastar;
using clk = std::chrono::steady_clock;

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        // snippet: orders
        std::vector<int> keys(1'000'000);
        std::iota(keys.begin(), keys.end(), 0);
        for (bool shuffled : {false, true}) {
            if (shuffled) {
                std::ranges::shuffle(keys, std::mt19937(1));
            }
            auto map = std::make_unique<std::unordered_map<int, int>>();
            for (int k : keys) {
                (*map)[k] = k;
            }
            auto start = clk::now();
            map.reset();
            using ms = std::chrono::duration<double, std::milli>;
            auto took = ms(clk::now() - start);
            std::println("inserted {}: destroyed in {:.1f} ms",
                         shuffled ? "in a random order" : "in order",
                         took.count());
        }
        // end snippet: orders
        return make_ready_future<>();
    });
}
