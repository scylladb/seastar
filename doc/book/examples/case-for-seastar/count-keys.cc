// count_keys() from chapter 1, in a complete program: every shard holds
// some keys, and shard 0 asks all of them how many they have.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sstring.hh>
#include <functional>
#include <print>
#include <unordered_set>

using namespace seastar;

// One shard's part of a store of keys.
class store {
    std::unordered_set<sstring> _keys;
public:
    void add(sstring key) {
        _keys.insert(std::move(key));
    }
    size_t size() const {
        return _keys.size();
    }
    future<> stop() {
        return make_ready_future<>();
    }
};

// snippet: count-keys
future<size_t> count_keys(sharded<store>& db) {
    return db.map_reduce0([] (const store& s) { return s.size(); },
                          size_t(0), std::plus<>());
}
// end snippet: count-keys

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        sharded<store> db;
        co_await db.start();
        // Shard i gets 100 + i keys.
        co_await db.invoke_on_all([] (store& s) {
            for (unsigned i = 0; i < 100 + this_shard_id(); i++) {
                s.add(fmt::format("key-{}-{}", this_shard_id(), i));
            }
        });
        auto keys = co_await count_keys(db);
        std::println("{} shards hold {} keys", this_smp_shard_count(), keys);
        co_await db.stop();
    });
}
