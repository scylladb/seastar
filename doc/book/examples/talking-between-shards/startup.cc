// Two ways to start a sharded service: on every shard, with an argument
// that differs from shard to shard (sharded_parameter), and on one shard
// only (start_single()). Run with two shards or more.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sstring.hh>
#include <fmt/format.h>
#include <print>

using namespace seastar;

// A service that greets, with a name that it's given when it starts.
class greeter {
    sstring _name;
public:
    explicit greeter(sstring name) : _name(std::move(name)) {}
    sstring greet() const { return "hello from " + _name; }
};

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: parameter
        // Each shard's instance gets its own name: the function runs on
        // each shard, and its result goes to that shard's constructor.
        sharded<greeter> greeters;
        co_await greeters.start(sharded_parameter([] {
            return sstring(fmt::format("shard {}", this_shard_id()));
        }));
        auto greetings = co_await greeters.map(
            [] (greeter& g) { return g.greet(); });
        for (auto& greeting : greetings) {
            std::println("{}", greeting);
        }
        co_await greeters.stop();
        // end snippet: parameter
        // snippet: single
        // Only shard 0 has an instance; the others reach it with
        // invoke_on().
        sharded<greeter> single;
        co_await single.start_single(sstring("the only instance"));
        co_await smp::submit_to(1, [&single] (this auto) -> future<> {
            std::println("shard 1 has an instance: {}",
                         single.local_is_initialized());
            std::println("shard 1 asks shard 0: {}",
                         co_await single.invoke_on(0,
                             [] (greeter& g) { return g.greet(); }));
            try {
                co_await single.invoke_on(1, [] (greeter&) {});
            } catch (no_sharded_instance_exception& e) {
                std::println("shard 1 asks shard 1: {}", e.what());
            }
        });
        co_await single.stop();
        // end snippet: single
    });
}
