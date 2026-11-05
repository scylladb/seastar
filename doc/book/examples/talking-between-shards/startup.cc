// How the instances of a sharded service get their constructors'
// arguments: the same value for all of them, or a value made on each
// shard, from that shard's instance of another service
// (sharded_parameter). And a service with only one instance
// (start_single()). Run with three shards.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sstring.hh>
#include <fmt/format.h>
#include <functional>
#include <print>
#include <unordered_set>

using namespace seastar;

// snippet: services
// A store of keys, with an instance on every shard, which holds the keys
// that belong to that shard.
class store {
    std::unordered_set<sstring> _keys;
public:
    void add(sstring key) { _keys.insert(std::move(key)); }
    size_t size() const { return _keys.size(); }
};

// Reports how many keys its shard has. It doesn't take the store, only a
// function that counts the keys, so it doesn't depend on the store's
// class, and a test can give it any function.
class reporter {
    sstring _what;
    std::function<size_t()> _count;
public:
    reporter(sstring what, std::function<size_t()> count)
        : _what(std::move(what)), _count(std::move(count)) {}
    sstring report() const {
        return fmt::format("shard {} has {} {}",
                           this_shard_id(), _count(), _what);
    }
};
// end snippet: services

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        sharded<store> stores;
        co_await stores.start();
        // Seven keys, spread over the shards in turn.
        for (int i = 0; i < 7; i++) {
            co_await stores.invoke_on(i % this_smp_shard_count(), [i] (store& s) {
                s.add(fmt::format("key{}", i));
            });
        }
        // snippet: parameter
        sharded<reporter> reporters;
        co_await reporters.start(
            // The same for every instance: each gets a copy.
            sstring("keys"),
            // Made on each shard, from that shard's store.
            sharded_parameter([] (store& s) {
                return std::function<size_t()>([&s] { return s.size(); });
            }, std::ref(stores)));
        auto reports = co_await reporters.map(
            [] (reporter& r) { return r.report(); });
        for (auto& report : reports) {
            std::println("{}", report);
        }
        co_await reporters.stop();
        // end snippet: parameter
        co_await stores.stop();

        // snippet: single
        // Only shard 0 has an instance; the others reach it with
        // invoke_on().
        sharded<store> single;
        co_await single.start_single();
        co_await smp::submit_to(1, [&single] (this auto) -> future<> {
            std::println("shard 1 has an instance: {}",
                         single.local_is_initialized());
            auto n = co_await single.invoke_on(0, [] (store& s) {
                s.add("from shard 1");
                return s.size();
            });
            std::println("shard 1 added a key on shard 0, which now has {}", n);
            try {
                co_await single.invoke_on(1, [] (store&) {});
            } catch (no_sharded_instance_exception& e) {
                std::println("shard 1 asks shard 1: {}", e.what());
            }
        });
        co_await single.stop();
        // end snippet: single
    });
}
