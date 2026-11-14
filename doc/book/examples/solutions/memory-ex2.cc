// Chapter 20, exercise 2: fragmentation. Allocates a thousand blocks of a
// megabyte, frees every other one, and then looks for the largest
// allocation that succeeds. Run it with one shard and 1 GB: -c1 -m1G.
#include <seastar/core/app-template.hh>
#include <seastar/core/memory.hh>
#include <memory>
#include <new>
#include <print>
#include <vector>

using namespace seastar;

void escape(void* p) {
    asm volatile("" : : "g"(p) : "memory");
}

// Whether an allocation of this many bytes succeeds now.
bool fits(size_t bytes) {
    try {
        std::unique_ptr<char[]> p(new char[bytes]);
        escape(p.get());
        return true;
    } catch (std::bad_alloc&) {
        return false;
    }
}

// The largest allocation that succeeds, to the nearest 64 KB.
size_t largest() {
    size_t low = 0, high = memory::stats().total_memory();
    while (high - low > 64 * 1024) {
        size_t mid = (low + high) / 2;
        (fits(mid) ? low : high) = mid;
    }
    return low;
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        // snippet: holes
        std::println("at the start: {} MB free, largest allocation {} KB",
                     memory::stats().free_memory() >> 20, largest() >> 10);
        std::vector<std::unique_ptr<char[]>> blocks;
        for (int i = 0; i < 1000; i++) {
            blocks.emplace_back(new char[1 << 20]);
            escape(blocks.back().get());
        }
        for (size_t i = 0; i < blocks.size(); i += 2) {
            blocks[i].reset();
        }
        std::println("after freeing every other block: {} MB free, "
                     "largest allocation {} KB",
                     memory::stats().free_memory() >> 20, largest() >> 10);
        // end snippet: holes
        return make_ready_future<>();
    });
}
