// Chapter 21, exercise 2: a pool of worker.cc's workers, in which functions
// with the same key always go to the same worker, as in Ceph's Crimson, so
// that they run in the order they were submitted. Run it with one shard:
// -c1.
#include <seastar/core/alien.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/noncopyable_function.hh>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <print>
#include <thread>
#include <type_traits>
#include <memory>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

// An operating-system thread that runs functions for the shards, one at a
// time, in the order they were submitted.
class worker {
    alien::instance& _alien;
    std::mutex _mutex;
    std::condition_variable _wake;
    std::deque<noncopyable_function<void ()>> _jobs;   // under _mutex
    bool _stopping = false;                             // under _mutex
    std::thread _thread;            // last: it starts running at once

    void run() {
        while (true) {
            noncopyable_function<void ()> job;
            {
                std::unique_lock lock(_mutex);
                _wake.wait(lock, [this] {
                    return _stopping || !_jobs.empty();
                });
                if (_jobs.empty()) {
                    return;         // stopping, and nothing left to do
                }
                job = std::move(_jobs.front());
                _jobs.pop_front();
            }
            job();
        }
    }
    void push(noncopyable_function<void ()> job) {
        {
            std::lock_guard lock(_mutex);
            _jobs.push_back(std::move(job));
        }
        _wake.notify_one();
    }
public:
    explicit worker(alien::instance& alien)
        : _alien(alien), _thread([this] { run(); }) {}

    // Runs func() in the worker thread, and returns what it returns. The
    // promise stays here, in this coroutine's frame, on the shard: the
    // worker only gets its address, and asks the shard to fulfill it.
    template <typename Func, typename T = std::invoke_result_t<Func>>
    future<T> submit(Func func) {
        promise<T> result;
        auto shard = this_shard_id();
        push([this, shard, &result, func = std::move(func)] () mutable {
            // In the worker thread: nothing of Seastar's but alien::.
            std::optional<T> value;
            std::exception_ptr error;
            try {
                value.emplace(func());
            } catch (...) {
                error = std::current_exception();
            }
            // run_on() only queues the message, and returns at once, so
            // the message must carry the value and the error with it.
            alien::run_on(_alien, shard, [&result, value = std::move(value),
                                          error] () mutable noexcept {
                // On the shard again.
                if (error) {
                    result.set_exception(error);
                } else {
                    result.set_value(std::move(*value));
                }
            });
        });
        co_return co_await result.get_future();
    }

    // Lets the worker finish what was submitted, and waits until it ends,
    // without blocking the shard: the thread's last job tells the shard.
    future<> stop() {
        co_await submit([] { return 0; });
        {
            std::lock_guard lock(_mutex);
            _stopping = true;
        }
        _wake.notify_one();
        _thread.join();             // ends right away: nothing is left
    }
};

// snippet: pool
// A pool of workers: a key chooses the worker.
class pool {
    std::vector<std::unique_ptr<worker>> _workers;
public:
    pool(alien::instance& alien, size_t n) {
        for (size_t i = 0; i < n; i++) {
            _workers.push_back(std::make_unique<worker>(alien));
        }
    }
    template <typename Func>
    auto submit(size_t key, Func func) {
        return _workers[key % _workers.size()]->submit(std::move(func));
    }
    future<> stop() {
        for (auto& w : _workers) {
            co_await w->stop();
        }
    }
};
// end snippet: pool

// snippet: test
// Submits eight functions with each of four keys, at once, each sleeping
// 50 ms, and checks that each key's functions ended in order.
future<> test(alien::instance& alien, size_t workers) {
    pool p(alien, workers);
    auto start = std::chrono::steady_clock::now();
    std::vector<std::vector<int>> ended(4);
    std::vector<future<>> all;
    for (int i = 0; i < 8; i++) {
        for (size_t key = 0; key < 4; key++) {
            all.push_back(p.submit(key, [i] {
                std::this_thread::sleep_for(50ms);
                return i;
            }).then([&ended, key] (int i) { ended[key].push_back(i); }));
        }
    }
    co_await when_all_succeed(all.begin(), all.end());
    auto took = std::chrono::steady_clock::now() - start;
    co_await p.stop();
    bool in_order = true;
    for (auto& e : ended) {
        in_order = in_order && std::ranges::is_sorted(e) && e.size() == 8;
    }
    using ms = std::chrono::duration<double, std::milli>;
    std::println("{} workers: in order: {}; {:.0f} ms", workers, in_order,
                 ms(took).count());
}
// end snippet: test

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [&app] () -> future<> {
        alien::instance& alien = app.alien();
        co_await test(alien, 4);
        co_await test(alien, 1);
    });
}
