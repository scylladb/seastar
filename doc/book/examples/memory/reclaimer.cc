// A cache that gives memory back: blobs of a megabyte, oldest first, and a
// reclaimer that drops the oldest when the shard's allocator runs low. The
// program adds 3,000 blobs to a shard with 1 GB, which without the
// reclaimer would fail with std::bad_alloc after about a thousand.
// Run it with one shard and 1 GB: -c1 -m1G.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/memory.hh>
#include <seastar/util/later.hh>
#include <cstddef>
#include <deque>
#include <memory>
#include <print>

using namespace seastar;

constexpr size_t blob_size = 1 << 20;

// snippet: cache
class blob_cache {
    std::deque<std::unique_ptr<char[]>> _blobs;     // the oldest first
    size_t _dropped = 0;
    // Called, in a fiber of its own, when the shard's free memory is low:
    // drops the oldest blobs until it has freed what the allocator asks.
    memory::reclaimer _reclaimer{[this] (memory::reclaimer::request r) {
        size_t freed = 0;
        while (freed < r.bytes_to_reclaim && !_blobs.empty()) {
            _blobs.pop_front();
            freed += blob_size;
            _dropped++;
        }
        return freed ? memory::reclaiming_result::reclaimed_something
                     : memory::reclaiming_result::reclaimed_nothing;
    }, memory::reclaimer_scope::async};
public:
    void add() {
        _blobs.emplace_back(new char[blob_size]);
    }
    size_t size() const { return _blobs.size(); }
    size_t dropped() const { return _dropped; }
};
// end snippet: cache

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: fill
        blob_cache cache;
        for (int i = 0; i < 3000; i++) {
            cache.add();
            // Let other fibers run, the reclaimer's among them.
            co_await yield();
        }
        std::println("{} blobs kept, {} dropped, {} MB free",
                     cache.size(), cache.dropped(),
                     memory::stats().free_memory() >> 20);
        // end snippet: fill
    });
}
