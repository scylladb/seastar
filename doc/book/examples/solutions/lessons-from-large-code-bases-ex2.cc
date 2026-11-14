// Exercise 2 of "Lessons from large Seastar code bases": a lock of every
// shard. Taken in parallel, two callers can deadlock; taken on shard 0
// first, they can't.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/sleep.hh>
#include <algorithm>
#include <chrono>
#include <print>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

struct shard_lock {
    semaphore sem{1};
    future<> stop() { return make_ready_future<>(); }
};

// Takes this shard's lock, or gives up after half a second, so that a
// deadlock ends. To reproduce the deadlock every time, a caller can be
// made slow to take one shard's lock: its request is delayed by 20 ms, as
// a busy shard or a reordered message would delay it.
future<bool> take(shard_lock& l, unsigned late_shard) {
    if (this_shard_id() == late_shard) {
        co_await sleep(20ms);
    }
    try {
        co_await l.sem.wait(semaphore::clock::now() + 500ms, 1);
        co_return true;
    } catch (semaphore_timed_out&) {
        co_return false;
    }
}

// Releases the locks that `held` says this caller got.
future<> release(sharded<shard_lock>& locks, std::vector<bool> held) {
    co_await locks.invoke_on_all([held] (shard_lock& l) {
        if (held[this_shard_id()]) {
            l.sem.signal(1);
        }
    });
}

// Every shard's lock at once: which of them this caller got.
future<std::vector<bool>> lock_all_in_parallel(sharded<shard_lock>& locks, unsigned late_shard) {
    return locks.map([late_shard] (shard_lock& l) {
        return take(l, late_shard);
    });
}

// Shard 0's lock first, then the others: a caller that has shard 0's has
// no competitor for the rest.
future<std::vector<bool>> lock_all_shard_0_first(sharded<shard_lock>& locks, unsigned late_shard) {
    bool first = co_await locks.invoke_on(0, [late_shard] (shard_lock& l) {
        return take(l, late_shard);
    });
    if (!first) {
        co_return std::vector<bool>(this_smp_shard_count(), false);
    }
    co_return co_await locks.map([late_shard] (shard_lock& l) {
        return this_shard_id() == 0 ? make_ready_future<bool>(true) : take(l, late_shard);
    });
}

// Locks every shard, holds the locks briefly, and says how many it got.
template <typename LockAll>
future<long> lock_hold_release(sharded<shard_lock>& locks, LockAll lock_all, unsigned late_shard) {
    auto held = co_await lock_all(locks, late_shard);
    co_await sleep(10ms);
    co_await release(locks, held);
    co_return std::ranges::count(held, true);
}

// Two callers at once: a is late to take shard 1's lock, and b shard 0's.
template <typename LockAll>
future<> race(sharded<shard_lock>& locks, LockAll lock_all, const char* name) {
    auto a = lock_hold_release(locks, lock_all, 1);
    auto b = lock_hold_release(locks, lock_all, 0);
    long got_a = co_await std::move(a);
    long got_b = co_await std::move(b);
    auto n = this_smp_shard_count();
    std::println("{}: a got {} of {} locks, b got {} of {}{}", name, got_a, n, got_b, n,
                 got_a < long(n) && got_b < long(n) ? ": a deadlock" : "");
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        sharded<shard_lock> locks;
        co_await locks.start();
        co_await race(locks, lock_all_in_parallel, "in parallel");
        co_await race(locks, lock_all_shard_0_first, "shard 0 first");
        co_await locks.stop();
    });
}
