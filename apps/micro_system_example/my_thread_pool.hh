/**
 * DAIL in Alibaba Group
 *
 */

#pragma once

#include <seastar/core/internal/poll.hh>
#include <seastar/core/internal/pollable_fd.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/util/std-compat.hh>
#include <boost/lockfree/queue.hpp>

class my_thread_pool;

class work_thread {
    struct work_item {
        unsigned _from_shard;

        explicit work_item(unsigned from_shard): _from_shard(from_shard) {}
        virtual ~work_item() = default;
        virtual void process() = 0;
        virtual void complete() = 0;
    };
    template <typename T>
    struct work_item_returning : work_item {
        seastar::noncopyable_function<T ()> _func;
        seastar::promise<T> _promise;
        boost::optional<T> _result{};

        work_item_returning(unsigned from_shard, seastar::noncopyable_function<T ()> func)
            : work_item(from_shard), _func(std::move(func)) {}
        void process() override { _result = this->_func(); }
        void complete() override { _promise.set_value(std::move(*_result)); }
        seastar::future<T> get_future() { return _promise.get_future(); }
    };
    std::unique_ptr<work_item> _work_item;
    seastar::writeable_eventfd _start_eventfd;
    seastar::posix_thread _worker;
    std::atomic<bool> _stopped = { false };
    seastar::reactor* _employer = nullptr;
public:
    ~work_thread();
private:
    explicit work_thread(unsigned worker_id);

    template <typename T>
    seastar::future<T> submit(seastar::reactor* employer, seastar::noncopyable_function<T ()> func) {
        _employer = employer;
        auto* wi = new work_item_returning<T>(seastar::this_shard_id(), std::move(func));
        auto fut = wi->get_future();
        _work_item.reset(wi);
        _start_eventfd.signal(1);
        return fut;
    }

    void work(unsigned worker_id);

    friend class my_thread_pool;
};

class my_thread_pool_pollfn;

class my_thread_pool {
    static unsigned _queue_length;
    static unsigned _worker_num;
    static int64_t _sleep_duration_in_microseconds;
    static std::unique_ptr<boost::lockfree::queue<work_thread*>> _threads;
    using completed_queue = boost::lockfree::queue<work_thread::work_item*>;
    struct completed_queue_deleter {
        void operator()(completed_queue* q) const;
    };
    static std::unique_ptr<completed_queue[], completed_queue_deleter> _completed;
    struct atomic_flag_deleter {
        void operator()(std::atomic<bool>* flags) const;
    };
    static std::unique_ptr<std::atomic<bool>[], atomic_flag_deleter> _reactors_idle;
    struct wt_pointer {
        work_thread* d;
        wt_pointer() : d(nullptr) {}
        wt_pointer(wt_pointer&& other) noexcept : d(other.d) {}
    };
public:
    static void configure();
    static void stop();

    template <typename T, typename Func>
    static seastar::future<T> submit_work(Func func, const bool create_worker_if_fail_acquire = true) {
        return seastar::do_with(wt_pointer(),
                [func=std::move(func), create_worker_if_fail_acquire] (wt_pointer &wp) mutable {
            return seastar::futurize_invoke([&wp, create_worker_if_fail_acquire] {
                if (!_threads->pop(wp.d)) {
                    if (create_worker_if_fail_acquire && _worker_num < _queue_length) {
                        wp.d = new work_thread(_worker_num++);
                        return seastar::make_ready_future<>();
                    }
                    return seastar::repeat([&wp] {
                        return seastar::sleep(std::chrono::microseconds(
                            _sleep_duration_in_microseconds)).then([&wp] {
                            return _threads->pop(wp.d)
                                ? seastar::stop_iteration::yes
                                : seastar::stop_iteration::no;
                        });
                    });
                }
                return seastar::make_ready_future<>();
            }).then([&wp, func=std::move(func)] () mutable {
                return wp.d->submit<T>(seastar::local_engine, std::move(func));
            });
        });
    }

private:
    static void complete_work_item(work_thread::work_item* wi);
    static void return_worker(work_thread* wt);

    static bool poll_queues();
    static bool pure_poll_queues();

    static void enter_interrupt_mode();
    static void exit_interrupt_mode();

    friend class work_thread;
    friend class my_thread_pool_pollfn;
};

/// my_thread_pool pollfn
class my_thread_pool_pollfn final : public seastar::pollfn {
public:
    bool poll() final {
        return my_thread_pool::poll_queues();
    }
    bool pure_poll() final {
        return my_thread_pool::pure_poll_queues();
    }
    bool try_enter_interrupt_mode() final {
        my_thread_pool::enter_interrupt_mode();
        if (poll()) {
            // raced
            my_thread_pool::exit_interrupt_mode();
            return false;
        }
        return true;
    }
    void exit_interrupt_mode() final {
        my_thread_pool::exit_interrupt_mode();
    }
};


