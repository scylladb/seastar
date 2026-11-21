// A shard's memory: how much it has, what a large allocation really takes,
// why a shard with 1 GB can't allocate 900 MB, and the warning that large
// allocations can log. Run it with one shard and 1 GB: -c1 -m1G.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/memory.hh>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <print>
#include <string_view>
#include <vector>

using namespace seastar;

// Keeps the compiler from removing an allocation that nothing reads.
void escape(void* p) {
    asm volatile("" : : "g"(p) : "memory");
}

// snippet: show
void show(std::string_view when) {
    auto s = memory::stats();
    std::println("{}: {} MB in all, {} MB free", when,
                 s.total_memory() >> 20, s.free_memory() >> 20);
}
// end snippet: show

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        show("at the start");

        // snippet: rounding
        // A large allocation takes a power of two of pages.
        {
            auto before = memory::stats().free_memory();
            std::unique_ptr<char[]> p(new char[600 * 1024]);
            escape(p.get());
            auto taken = before - memory::stats().free_memory();
            std::println("600 KB took {} KB", taken >> 10);
        }
        // end snippet: rounding

        // snippet: too-big
        // An allocation must fit in one free block, however much memory
        // is free in all.
        for (size_t mb : {900, 600, 500}) {
            try {
                std::unique_ptr<char[]> p(new char[mb << 20]);
                escape(p.get());
                std::println("{} MB: allocated", mb);
            } catch (std::bad_alloc&) {
                std::println("{} MB: std::bad_alloc", mb);
            }
        }
        // end snippet: too-big

        // snippet: warning
        // Log a warning, with a backtrace, for every allocation over
        // 128 KB; this vector of a megabyte gets one.
        memory::set_large_allocation_warning_threshold(128 * 1024);
        std::vector<char> big(1 << 20);
        escape(big.data());
        memory::set_large_allocation_warning_threshold(
            std::numeric_limits<size_t>::max());
        // end snippet: warning
        show("at the end");
        co_return;
    });
}
