// Chapter 21, exercise 1: worker.cc's worker, with a semaphore that lets no
// more than 16 functions wait for it, as Redpanda's worker does.
// Run it with one shard: -c1.
#include <seastar/core/alien.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/smp.hh>
#include <seastar/util/noncopyable_function.hh>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <print>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

using namespace seastar;
using namespace std::chrono_literals;

// An operating-system thread that runs functions for the shards, one at a
// time, in the order they were submitted.
class worker {
    alien::instance& _seastar;
    std::mutex _mutex;
    std::condition_variable _wake;
    std::deque<noncopyable_function<void ()>> _jobs;   // under _mutex
    bool _stopping = false;                             // under _mutex
    size_t _most_waiting = 0;                           // under _mutex
    // On the shard: room for the functions submitted and not yet done.
    semaphore _room{16};
    shard_id _home = this_shard_id();   // where it was made, and stops
    promise<> _stopped;                 // on _home: the thread has ended
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
                    break;          // stopping, and nothing left to do
                }
                job = std::move(_jobs.front());
                _jobs.pop_front();
            }
            job();
        }
        // The thread's last act: tell the shard that it's ending.
        alien::run_on(_seastar, _home, [this] () noexcept {
            _stopped.set_value();
        });
    }
    void push(noncopyable_function<void ()> job) {
        {
            std::lock_guard lock(_mutex);
            if (_stopping) {
                throw std::runtime_error("the worker is stopping");
            }
            _jobs.push_back(std::move(job));
            _most_waiting = std::max(_most_waiting, _jobs.size());
        }
        _wake.notify_one();
    }
public:
    explicit worker(alien::instance& seastar)
        : _seastar(seastar), _thread([this] { run(); }) {}

    // Runs func() in the worker thread, and returns what it returns. The
    // promise stays here, in this coroutine's frame, on the shard: the
    // worker only gets its address, and asks the shard to fulfill it.
    template <typename Func, typename T = std::invoke_result_t<Func>>
    future<T> submit(Func func) {
        // Wait for room, and keep it until the function is done.
        // snippet: room
        auto room = co_await get_units(_room, 1);
        // end snippet: room
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
            alien::run_on(_seastar, shard, [&result, value = std::move(value),
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

    // Refuses new functions, lets the worker finish those already
    // submitted, and waits until its thread ends, without blocking the
    // shard. Must run on the shard that made the worker.
    future<> stop() {
        {
            std::lock_guard lock(_mutex);
            _stopping = true;
        }
        _wake.notify_one();
        co_await _stopped.get_future();
        _thread.join();             // it's ending, so this hardly waits
    }
    size_t most_waiting() {
        std::lock_guard lock(_mutex);
        return _most_waiting;
    }
};

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [&app] () -> future<> {
        // snippet: test
        worker w(app.alien());
        auto start = std::chrono::steady_clock::now();
        // A hundred functions, submitted at once, each sleeping 10 ms.
        std::vector<future<int>> results;
        for (int i = 0; i < 100; i++) {
            results.push_back(w.submit([i] {
                std::this_thread::sleep_for(10ms);
                return i;
            }));
        }
        int sum = 0;
        for (auto& r : results) {
            sum += co_await std::move(r);
        }
        auto took = std::chrono::steady_clock::now() - start;
        co_await w.stop();
        using ms = std::chrono::duration<double, std::milli>;
        std::println("sum {}; at most {} functions waited; {:.0f} ms", sum,
                     w.most_waiting(), ms(took).count());
        // end snippet: test
    });
}
