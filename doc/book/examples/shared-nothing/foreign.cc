// Handing an object to another shard: shard 1 builds a table, and gives it
// to shard 0 in a foreign_ptr, which destroys the table back on shard 1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sstring.hh>
#include <fmt/format.h>
#include <memory>
#include <print>
#include <vector>

using namespace seastar;

// snippet: table
struct table {
    std::vector<sstring> rows;
    ~table() {
        std::println("the table is destroyed on shard {}", this_shard_id());
    }
};

// Builds a table on the given shard, and returns it to the caller's shard.
future<foreign_ptr<std::unique_ptr<table>>> build_table_on(unsigned shard) {
    return smp::submit_to(shard, [] {
        auto t = std::make_unique<table>();
        for (int i = 0; i < 3; i++) {
            t->rows.push_back(fmt::format("row {}, made on shard {}", i,
                                          this_shard_id()));
        }
        return make_foreign(std::move(t));
    });
}
// end snippet: table

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        if (this_smp_shard_count() < 2) {
            throw std::runtime_error("run with at least two shards, -c2");
        }
        // snippet: use
        auto t = co_await build_table_on(1);
        std::println("shard {} got a table that belongs to shard {}:",
                     this_shard_id(), t.get_owner_shard());
        for (const auto& row : t->rows) {
            std::println("  {}", row);
        }
        // Destroy the table on its own shard, and wait until it's done.
        co_await t.destroy();
        std::println("back on shard {}", this_shard_id());
        // end snippet: use
    });
}
