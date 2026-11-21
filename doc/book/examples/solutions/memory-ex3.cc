// Chapter 20, exercise 3: reclaimer.cc with blobs of 1 KB, two million of
// them, dropped oldest first or, with the argument "random", at random.
// Run it with one shard and 1 GB: -c1 -m1G, or -c1 -m1G random.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/memory.hh>
#include <seastar/util/later.hh>
#include <cstddef>
#include <deque>
#include <memory>
#include <print>
#include <random>
#include <string_view>
#include <utility>

using namespace seastar;

constexpr size_t blob_size = 1024;

// snippet: cache
class blob_cache {
    std::deque<std::unique_ptr<char[]>> _blobs;     // the oldest first
    bool _random;
    std::minstd_rand _generator;
    size_t _dropped = 0;
    size_t _calls = 0;
    void drop() {
        if (_random) {
            // Swap a random blob to the end, and drop it from there.
            auto i = _generator() % _blobs.size();
            std::swap(_blobs[i], _blobs.back());
            _blobs.pop_back();
        } else {
            _blobs.pop_front();
        }
        _dropped++;
    }
    memory::reclaimer _reclaimer{[this] (memory::reclaimer::request r) {
        _calls++;
        size_t freed = 0;
        while (freed < r.bytes_to_reclaim && !_blobs.empty()) {
            drop();
            freed += blob_size;
        }
        return freed ? memory::reclaiming_result::reclaimed_something
                     : memory::reclaiming_result::reclaimed_nothing;
    }, memory::reclaimer_scope::async};
public:
    explicit blob_cache(bool random) : _random(random) {}
    void add() {
        _blobs.emplace_back(new char[blob_size]);
    }
    size_t size() const { return _blobs.size(); }
    size_t dropped() const { return _dropped; }
    size_t calls() const { return _calls; }
    bool random() const { return _random; }
};
// end snippet: cache

int main(int argc, char** argv) {
    // The last argument, if it's "random", is the program's own.
    bool random = argc > 1 && std::string_view(argv[argc - 1]) == "random";
    if (random) {
        argc--;
    }
    app_template app;
    return app.run(argc, argv, [random] () -> future<> {
        // The lambda's captures are gone after its first co_await (the
        // Lambda Coroutine Fiasco, chapter 8), so only the cache keeps
        // random.
        blob_cache cache(random);
        for (int i = 0; i < 2'000'000; i++) {
            cache.add();
            // Let the reclaimer run, every hundred blobs.
            if (i % 100 == 0) {
                co_await yield();
            }
        }
        std::println("{}: kept {} ({} MB), dropped {}, {} calls, {} MB free",
                     cache.random() ? "random" : "oldest",
                     cache.size(), (cache.size() * blob_size) >> 20,
                     cache.dropped(), cache.calls(),
                     memory::stats().free_memory() >> 20);
    });
}
