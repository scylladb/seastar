#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/smp.hh>
#include <seastar/util/spinlock.hh>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <iostream>
#include <cstring>

using namespace seastar;

namespace seastar {

namespace util {

// Spin lock implementation.
// BasicLockable.
// Async-signal safe.
// unlock() "synchronizes with" lock().
class spinlock_false_sharing {
    std::atomic<bool> _busy = { false };
public:
    spinlock_false_sharing() = default;
    spinlock_false_sharing(const spinlock_false_sharing&) = delete;
    ~spinlock_false_sharing() { assert(!_busy.load(std::memory_order_relaxed)); }
    bool try_lock() noexcept {
        return !_busy.exchange(true, std::memory_order_acquire);
    }
    void lock() noexcept {
        while (_busy.exchange(true, std::memory_order_acquire)) {
            while (_busy.load(std::memory_order_relaxed)) {
                internal::cpu_relax();
            }
        }
    }
    void unlock() noexcept {
        _busy.store(false, std::memory_order_release);
    }
};

}

}

// False sharing
struct BadObject {
    util::spinlock_false_sharing lock;
    std::atomic<int> counter{0};
};

// No false sharing
struct GoodObject {
    util::spinlock lock;
    std::atomic<int> counter{0};
};

void worker_bad(BadObject& obj, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        obj.lock.lock();
        obj.counter.fetch_add(1, std::memory_order_relaxed);
        obj.lock.unlock();
    }
}

void worker_good(GoodObject& obj, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        obj.lock.lock();
        obj.counter.fetch_add(1, std::memory_order_relaxed);
        obj.lock.unlock();
    }
}

future<> run_benchmark(int nthreads, int iterations, const char* name) {
    return async([=] {
        std::cout << "bench: " << name << "\n";

        auto start = std::chrono::steady_clock::now();

        if (strcmp(name, "false sharing") == 0) {
            BadObject obj;
            std::vector<std::thread> threads;

            for (int t = 0; t < nthreads; ++t) {
                threads.emplace_back([&obj, iterations]() {
                    worker_bad(obj, iterations);
                });
            }

            for (auto& th : threads) th.join();

            auto end = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            double ops_per_sec = (nthreads * iterations * 1.0) / (duration.count() / 1e6);

            std::cout << "  ops: " << ops_per_sec/1e6 << "M\n";
            std::cout << "  counter: " << obj.counter << " (expected: "
                      << nthreads * iterations << ")\n";
            std::cout << "  duration: " << duration.count() << "us\n\n";

        } else {
            GoodObject obj;
            std::vector<std::thread> threads;

            for (int t = 0; t < nthreads; ++t) {
                threads.emplace_back([&obj, iterations]() {
                    worker_good(obj, iterations);
                });
            }

            for (auto& th : threads) th.join();

            auto end = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
            double ops_per_sec = (nthreads * iterations * 1.0) / (duration.count() / 1e6);

            std::cout << "  ops: " << ops_per_sec/1e6 << "M\n";
            std::cout << "  counter: " << obj.counter << " (expected: "
                      << nthreads * iterations << ")\n";
            std::cout << "  duration: " << duration.count() << "us\n\n";
        }
    });
}

future<int> main_func() {
    static int run_count = 0;
    if (run_count++ == 0) {
        return run_benchmark(8, 1000000, "false sharing").then([] {
            return run_benchmark(8, 1000000, "64-byte aligned");
        }).then([] { return 0; });
    }
    return make_ready_future<int>(0);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, main_func);
}
