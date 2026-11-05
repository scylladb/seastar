// Sharing a Seastar pointer between shards: every shard copies the same
// lw_shared_ptr, and destroys the copies, at the same time. Its reference
// count isn't atomic, so the shards' updates of it collide.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/smp.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <print>
#include <vector>

using namespace seastar;

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: wrong
        auto data = make_lw_shared<std::vector<int>>(1000);
        // Wrong: every shard copies data, a pointer that belongs to shard 0.
        co_await smp::invoke_on_all([&data] () -> future<> {
            std::vector<lw_shared_ptr<std::vector<int>>> copies;
            for (int round = 0; round < 10'000; round++) {
                for (int i = 0; i < 100; i++) {
                    copies.push_back(data);
                }
                copies.clear();
                co_await coroutine::maybe_yield();
            }
        });
        std::println("{} shards: use count {}, should be 1",
                     this_smp_shard_count(), data.use_count());
        // end snippet: wrong
    });
}
