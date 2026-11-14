// Chapter 24, exercise 4: co_await on_shard(n), which moves the rest of a
// coroutine to shard n, and what it breaks: the coroutine's caller, too,
// continues on shard n. Don't use it. Run it with two shards: -c2. It
// prints to stderr, which isn't buffered, so that its lines come out even
// if it aborts.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/smp.hh>
#include <coroutine>
#include <print>

using namespace seastar;

// snippet: on-shard
// co_await on_shard(n): resumes the coroutine on shard n, by sending a
// message there that resumes it.
class on_shard {
    unsigned _shard;
public:
    explicit on_shard(unsigned shard) : _shard(shard) {}
    bool await_ready() const noexcept {
        return _shard == this_shard_id();
    }
    void await_suspend(std::coroutine_handle<> h) noexcept {
        (void)smp::submit_to(_shard, [h] {
            h.resume();
        });
    }
    void await_resume() noexcept {}
};

future<> hop() {
    std::println(stderr, "hop: starts on shard {}", this_shard_id());
    co_await on_shard(1);
    std::println(stderr, "hop: continues on shard {}", this_shard_id());
}

future<> caller() {
    std::println(stderr, "caller: on shard {}", this_shard_id());
    co_await hop();
    std::println(stderr, "caller: after hop(), on shard {}",
                 this_shard_id());
    // end snippet: on-shard
    // Without this, main()'s function would end on shard 1 too, and
    // Seastar, which must be stopped from shard 0, aborts.
    co_await on_shard(0);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        return caller();
    });
}
