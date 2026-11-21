// A worker thread, outside Seastar, for code that blocks or runs too long:
// a shard submits a function, the worker runs it, and alien::run_on()
// brings the result back to the shard, while the shard goes on serving.
// Run it with one shard: -c1.
#include <seastar/core/alien.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/timer.hh>
#include <seastar/util/later.hh>
#include <seastar/util/noncopyable_function.hh>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <print>
#include <thread>
#include <type_traits>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: worker
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
    // end snippet: worker

    // snippet: submit
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
    // end snippet: submit

    // snippet: stop
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
    // end snippet: stop
};

// Counts the primes below n, the slow way: a long computation.
int count_primes(int n) {
    int count = 0;
    for (int i = 2; i < n; i++) {
        bool prime = true;
        for (int d = 2; d * d <= i; d++) {
            if (i % d == 0) {
                prime = false;
                break;
            }
        }
        count += prime;
    }
    return count;
}

// The two jobs: a call that blocks, such as a library's that waits for the
// network, and a long computation.
int blocking_call() {
    std::this_thread::sleep_for(300ms);
    return 42;
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [&app] () -> future<> {
        // A timer that fires every millisecond, while the shard is free to
        // run it: the number of times it fired shows how free it was.
        int ticks = 0;
        timer<> ticker([&ticks] { ticks++; });
        ticker.arm_periodic(1ms);

        // snippet: use
        worker w(app.alien());
        int blocked = co_await w.submit(blocking_call);
        int primes = co_await w.submit([] { return count_primes(5'000'000); });
        co_await w.stop();
        // end snippet: use
        std::println("in the worker: got {} and {} primes, and the timer "
                     "fired {} times", blocked, primes, ticks);

        // The same jobs, called right here, on the shard.
        ticks = 0;
        blocked = blocking_call();
        primes = count_primes(5'000'000);
        co_await yield();       // let the timer fire, if it's due
        std::println("on the shard: got {} and {} primes, and the timer "
                     "fired {} times", blocked, primes, ticks);
    });
}
