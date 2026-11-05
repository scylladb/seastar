// What each shard owns: a thread, pinned to one CPU, and a part of the
// machine's memory.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/smp.hh>
#include <print>
#include <sched.h>

using namespace seastar;

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: owns
        co_await smp::invoke_on_all([] {
            std::println("shard {} of {}: on CPU {}, with {} MB of memory",
                         this_shard_id(), this_smp_shard_count(), sched_getcpu(),
                         memory::stats().total_memory() >> 20);
        });
        // end snippet: owns
    });
}
