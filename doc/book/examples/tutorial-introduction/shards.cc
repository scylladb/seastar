// Runs a function on every shard: each one says hello.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/smp.hh>
#include <print>

using namespace seastar;

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: hello-all
        std::println("Seastar is running {} shards", this_smp_shard_count());
        co_await smp::invoke_on_all([] {
            std::println("Hello from shard {}", this_shard_id());
        });
        // end snippet: hello-all
    });
}
