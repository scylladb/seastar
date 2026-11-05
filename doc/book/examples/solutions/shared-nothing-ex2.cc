// Chapter 13, exercise 2: a table in an lw_shared_ptr, which shards 0 and 2
// both hold, through two foreign_ptrs that copy() made.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sstring.hh>
#include <chrono>
#include <print>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

struct table {
    std::vector<sstring> rows;
    ~table() {
        std::println("the table is destroyed on shard {}", this_shard_id());
    }
};

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        if (this_smp_shard_count() < 3) {
            throw std::runtime_error("run with at least three shards, -c3");
        }
        // snippet: copy
        auto t = co_await smp::submit_to(1, [] {
            auto t = make_lw_shared<table>();
            t->rows = {"a", "b"};
            return make_foreign(std::move(t));
        });
        auto t2 = co_await t.copy();
        std::println("shard 0 has two pointers, owned by shards {} and {}",
                     t.get_owner_shard(), t2.get_owner_shard());
        co_await smp::submit_to(2, [t2 = std::move(t2)] () -> future<> {
            std::println("shard 2 reads {} rows, and waits",
                         t2->rows.size());
            co_await sleep(100ms);
        });
        std::println("shard 0 drops its pointer");
        co_await t.destroy();
        std::println("shard 0 is done");
        // end snippet: copy
    });
}
