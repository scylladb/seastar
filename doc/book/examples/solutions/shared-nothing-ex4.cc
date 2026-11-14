// Chapter 13, exercise 4: how much of its memory can one shard allocate at
// once, and how much is free on each shard afterwards?
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/smp.hh>
#include <print>
#include <vector>

using namespace seastar;

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: allocate
        // Each attempt starts with all of shard 0's memory free.
        for (size_t mb : {512, 900, 2048}) {
            try {
                std::vector<char> v(mb << 20, 1);
                std::println("shard 0 allocated {} MB", mb);
            } catch (std::bad_alloc&) {
                std::println("shard 0 couldn't allocate {} MB", mb);
            }
        }
        // Keep 512 MB on shard 0, and see what's left on every shard.
        std::vector<char> kept(512 << 20, 1);
        co_await smp::invoke_on_all([] {
            std::println("shard {}: {} MB free", this_shard_id(),
                         memory::stats().free_memory() >> 20);
        });
        // end snippet: allocate
    });
}
