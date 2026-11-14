// What freeing memory on another shard costs: shard 0 allocates a million
// objects of 64 bytes, and frees them itself; then other shards, one, two
// and three at once, each free a million of shard 0's objects, and the
// frees are timed. Run it with four shards: --smp 4.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/smp.hh>
#include <chrono>
#include <print>
#include <vector>

using namespace seastar;
using clk = std::chrono::steady_clock;

constexpr size_t count = 1'000'000;

std::vector<char*> allocate() {
    std::vector<char*> objects(count);
    for (auto& p : objects) {
        p = new char[64];
    }
    return objects;
}

// Frees the objects, and returns how long each free took, in nanoseconds.
double free_all(std::vector<char*>& objects) {
    auto start = clk::now();
    for (auto p : objects) {
        delete[] p;
    }
    return std::chrono::duration<double, std::nano>(clk::now() - start)
           .count() / count;
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: compare
        auto objects = allocate();
        std::println("freed by shard 0, their owner: {:.1f} ns each",
                     free_all(objects));
        // Shards 1 to n free a million of shard 0's objects each, at once.
        for (unsigned n = 1; n < this_smp_shard_count(); n++) {
            std::vector<std::vector<char*>> batches(n + 1);
            for (unsigned s = 1; s <= n; s++) {
                batches[s] = allocate();
            }
            std::vector<double> ns(n + 1);
            co_await smp::invoke_on_others([&] {
                auto s = this_shard_id();
                if (s <= n) {
                    ns[s] = free_all(batches[s]);
                }
            });
            std::println("freed by {} other shard{} at once: {:.1f} ns each",
                         n, n > 1 ? "s" : "", ns[1]);
        }
        // end snippet: compare
    });
}
