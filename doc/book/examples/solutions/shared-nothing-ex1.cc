// Chapter 13, exercise 1: shard 1 returns its rows by value, instead of a
// foreign_ptr to a table, and shard 0 frees the strings.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sstring.hh>
#include <fmt/format.h>
#include <print>
#include <vector>

using namespace seastar;

// snippet: rows
future<std::vector<sstring>> build_rows_on(unsigned shard) {
    return smp::submit_to(shard, [] {
        std::vector<sstring> rows;
        for (int i = 0; i < 3; i++) {
            rows.push_back(fmt::format("row {}, made on shard {}", i,
                                       this_shard_id()));
        }
        return rows;
    });
}
// end snippet: rows

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        if (this_smp_shard_count() < 2) {
            throw std::runtime_error("run with at least two shards, -c2");
        }
        // snippet: free
        auto rows = co_await build_rows_on(1);
        std::println("shard {} got {} rows", this_shard_id(), rows.size());
        auto before = memory::stats().cross_cpu_frees();
        rows.clear();
        rows.shrink_to_fit();
        std::println("destroying them on shard 0 took {} cross-shard frees",
                     memory::stats().cross_cpu_frees() - before);
        // end snippet: free
    });
}
