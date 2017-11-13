/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright 2014 Cloudius Systems
 */

#ifndef REACTOR_HH_
#define REACTOR_HH_

#include "seastar.hh"
#include "iostream.hh"
#include "aligned_buffer.hh"
#include "cacheline.hh"
#include "circular_buffer_fixed_capacity.hh"
#include <memory>
#include <type_traits>
#include <libaio.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unordered_map>
#include <netinet/ip.h>
#include <cstring>
#include <cassert>
#include <stdexcept>
#include <iostream>
#include <unistd.h>
#include <vector>
#include <queue>
#include <algorithm>
#include <thread>
#include <system_error>
#include <chrono>
#include <ratio>
#include <atomic>
#include <experimental/optional>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/optional.hpp>
#include <boost/program_options.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/container/static_vector.hpp>
#include <set>
#include "util/eclipse.hh"
#include "future.hh"
#include "posix.hh"
#include "apply.hh"
#include "sstring.hh"
#include "deleter.hh"
#include "net/api.hh"
#include "temporary_buffer.hh"
#include "circular_buffer.hh"
#include "file.hh"
#include "semaphore.hh"
#include "fair_queue.hh"
#include "core/scattered_message.hh"
#include "core/enum.hh"
#include "core/memory.hh"
#include <boost/range/irange.hpp>
#include "timer.hh"
#include "condition-variable.hh"
#include "util/log.hh"
#include "lowres_clock.hh"
#include "manual_clock.hh"
#include "core/metrics_registration.hh"
#include "scheduling.hh"

#ifdef HAVE_OSV
#include <osv/sched.hh>
#include <osv/mutex.h>
#include <osv/condvar.h>
#include <osv/newpoll.hh>
#endif

extern "C" int _Unwind_RaiseException(void *h);

namespace seastar {

using shard_id = unsigned;


class reactor;
class pollable_fd;
class pollable_fd_state;

class pollable_fd_state {
public:
    struct speculation {
        int events = 0;
        explicit speculation(int epoll_events_guessed = 0) : events(epoll_events_guessed) {}
    };
    ~pollable_fd_state();
    explicit pollable_fd_state(file_desc fd, speculation speculate = speculation())
        : fd(std::move(fd)), events_known(speculate.events) {}
    pollable_fd_state(const pollable_fd_state&) = delete;
    void operator=(const pollable_fd_state&) = delete;
    void speculate_epoll(int events) { events_known |= events; }
    file_desc fd;
    int events_requested = 0; // wanted by pollin/pollout promises
    int events_epoll = 0;     // installed in epoll
    int events_known = 0;     // returned from epoll
    promise<> pollin;
    promise<> pollout;
    friend class reactor;
    friend class pollable_fd;
};

inline
size_t iovec_len(const std::vector<iovec>& iov)
{
    size_t ret = 0;
    for (auto&& e : iov) {
        ret += e.iov_len;
    }
    return ret;
}

class pollable_fd {
public:
    using speculation = pollable_fd_state::speculation;
    pollable_fd(file_desc fd, speculation speculate = speculation())
        : _s(std::make_unique<pollable_fd_state>(std::move(fd), speculate)) {}
public:
    pollable_fd(pollable_fd&&) = default;
    pollable_fd& operator=(pollable_fd&&) = default;
    future<size_t> read_some(char* buffer, size_t size);
    future<size_t> read_some(uint8_t* buffer, size_t size);
    future<size_t> read_some(const std::vector<iovec>& iov);
    future<> write_all(const char* buffer, size_t size);
    future<> write_all(const uint8_t* buffer, size_t size);
    future<size_t> write_some(net::packet& p);
    future<> write_all(net::packet& p);
    future<> readable();
    future<> writeable();
    void abort_reader(std::exception_ptr ex);
    void abort_writer(std::exception_ptr ex);
    future<pollable_fd, socket_address> accept();
    future<size_t> sendmsg(struct msghdr *msg);
    future<size_t> recvmsg(struct msghdr *msg);
    future<size_t> sendto(socket_address addr, const void* buf, size_t len);
    file_desc& get_file_desc() const { return _s->fd; }
    void shutdown(int how) { _s->fd.shutdown(how); }
    void close() { _s.reset(); }
protected:
    int get_fd() const { return _s->fd.get(); }
    friend class reactor;
    friend class readable_eventfd;
    friend class writeable_eventfd;
private:
    std::unique_ptr<pollable_fd_state> _s;
};

}

namespace std {

template <>
struct hash<::sockaddr_in> {
    size_t operator()(::sockaddr_in a) const {
        return a.sin_port ^ a.sin_addr.s_addr;
    }
};

}

bool operator==(const ::sockaddr_in a, const ::sockaddr_in b);

namespace seastar {

class network_stack_registrator {
public:
    using options = boost::program_options::variables_map;
    explicit network_stack_registrator(sstring name,
            boost::program_options::options_description opts,
            std::function<future<std::unique_ptr<network_stack>> (options opts)> factory,
            bool make_default = false);
};

class writeable_eventfd;

class readable_eventfd {
    pollable_fd _fd;
public:
    explicit readable_eventfd(size_t initial = 0) : _fd(try_create_eventfd(initial)) {}
    readable_eventfd(readable_eventfd&&) = default;
    writeable_eventfd write_side();
    future<size_t> wait();
    int get_write_fd() { return _fd.get_fd(); }
private:
    explicit readable_eventfd(file_desc&& fd) : _fd(std::move(fd)) {}
    static file_desc try_create_eventfd(size_t initial);

    friend class writeable_eventfd;
};

class writeable_eventfd {
    file_desc _fd;
public:
    explicit writeable_eventfd(size_t initial = 0) : _fd(try_create_eventfd(initial)) {}
    writeable_eventfd(writeable_eventfd&&) = default;
    readable_eventfd read_side();
    void signal(size_t nr);
    int get_read_fd() { return _fd.get(); }
private:
    explicit writeable_eventfd(file_desc&& fd) : _fd(std::move(fd)) {}
    static file_desc try_create_eventfd(size_t initial);

    friend class readable_eventfd;
};

// The reactor_notifier interface is a simplified version of Linux's eventfd
// interface (with semaphore behavior off, and signal() always signaling 1).
//
// A call to signal() causes an ongoing wait() to invoke its continuation.
// If no wait() is ongoing, the next wait() will continue immediately.
class reactor_notifier {
public:
    virtual future<> wait() = 0;
    virtual void signal() = 0;
    virtual ~reactor_notifier() {}
};

class thread_pool;
class smp;

class syscall_work_queue {
    static constexpr size_t queue_length = 128;
    struct work_item;
    using lf_queue = boost::lockfree::spsc_queue<work_item*,
                            boost::lockfree::capacity<queue_length>>;
    lf_queue _pending;
    lf_queue _completed;
    writeable_eventfd _start_eventfd;
    semaphore _queue_has_room = { queue_length };
    struct work_item {
        virtual ~work_item() {}
        virtual void process() = 0;
        virtual void complete() = 0;
    };
    template <typename T, typename Func>
    struct work_item_returning :  work_item {
        Func _func;
        promise<T> _promise;
        boost::optional<T> _result;
        work_item_returning(Func&& func) : _func(std::move(func)) {}
        virtual void process() override { _result = this->_func(); }
        virtual void complete() override { _promise.set_value(std::move(*_result)); }
        future<T> get_future() { return _promise.get_future(); }
    };
public:
    syscall_work_queue();
    template <typename T, typename Func>
    future<T> submit(Func func) {
        auto wi = std::make_unique<work_item_returning<T, Func>>(std::move(func));
        auto fut = wi->get_future();
        submit_item(std::move(wi));
        return fut;
    }
private:
    void work();
    // Scans the _completed queue, that contains the requests already handled by the syscall thread,
    // effectively opening up space for more requests to be submitted. One consequence of this is
    // that from the reactor's point of view, a request is not considered handled until it is
    // removed from the _completed queue.
    //
    // Returns the number of requests handled.
    unsigned complete();
    void submit_item(std::unique_ptr<syscall_work_queue::work_item> wi);

    friend class thread_pool;
};

class smp_message_queue {
    static constexpr size_t queue_length = 128;
    static constexpr size_t batch_size = 16;
    static constexpr size_t prefetch_cnt = 2;
    struct work_item;
    struct lf_queue_remote {
        reactor* remote;
    };
    using lf_queue_base = boost::lockfree::spsc_queue<work_item*,
                            boost::lockfree::capacity<queue_length>>;
    // use inheritence to control placement order
    struct lf_queue : lf_queue_remote, lf_queue_base {
        lf_queue(reactor* remote) : lf_queue_remote{remote} {}
        void maybe_wakeup();
    };
    lf_queue _pending;
    lf_queue _completed;
    struct alignas(seastar::cache_line_size) {
        size_t _sent = 0;
        size_t _compl = 0;
        size_t _last_snt_batch = 0;
        size_t _last_cmpl_batch = 0;
        size_t _current_queue_length = 0;
    };
    // keep this between two structures with statistics
    // this makes sure that they have at least one cache line
    // between them, so hw prefecther will not accidentally prefetch
    // cache line used by aother cpu.
    metrics::metric_groups _metrics;
    struct alignas(seastar::cache_line_size) {
        size_t _received = 0;
        size_t _last_rcv_batch = 0;
    };
    struct work_item {
        virtual ~work_item() {}
        virtual future<> process() = 0;
        virtual void complete() = 0;
    };
    template <typename Func>
    struct async_work_item : work_item {
        Func _func;
        using futurator = futurize<std::result_of_t<Func()>>;
        using future_type = typename futurator::type;
        using value_type = typename future_type::value_type;
        std::experimental::optional<value_type> _result;
        std::exception_ptr _ex; // if !_result
        typename futurator::promise_type _promise; // used on local side
        async_work_item(Func&& func) : _func(std::move(func)) {}
        virtual future<> process() override {
            try {
                return futurator::apply(this->_func).then_wrapped([this] (auto&& f) {
                    try {
                        _result = f.get();
                    } catch (...) {
                        _ex = std::current_exception();
                    }
                });
            } catch (...) {
                _ex = std::current_exception();
                return make_ready_future();
            }
        }
        virtual void complete() override {
            if (_result) {
                _promise.set_value(std::move(*_result));
            } else {
                // FIXME: _ex was allocated on another cpu
                _promise.set_exception(std::move(_ex));
            }
        }
        future_type get_future() { return _promise.get_future(); }
    };
    union tx_side {
        tx_side() {}
        ~tx_side() {}
        void init() { new (&a) aa; }
        struct aa {
            std::deque<work_item*> pending_fifo;
        } a;
    } _tx;
    std::vector<work_item*> _completed_fifo;
public:
    smp_message_queue(reactor* from, reactor* to);
    template <typename Func>
    futurize_t<std::result_of_t<Func()>> submit(Func&& func) {
        auto wi = std::make_unique<async_work_item<Func>>(std::forward<Func>(func));
        auto fut = wi->get_future();
        submit_item(std::move(wi));
        return fut;
    }
    void start(unsigned cpuid);
    template<size_t PrefetchCnt, typename Func>
    size_t process_queue(lf_queue& q, Func process);
    size_t process_incoming();
    size_t process_completions();
    void stop();
private:
    void work();
    void submit_item(std::unique_ptr<work_item> wi);
    void respond(work_item* wi);
    void move_pending();
    void flush_request_batch();
    void flush_response_batch();
    bool has_unflushed_responses() const;
    bool pure_poll_rx() const;
    bool pure_poll_tx() const;

    friend class smp;
};

class thread_pool {
    uint64_t _aio_threaded_fallbacks = 0;
#ifndef HAVE_OSV
    // FIXME: implement using reactor_notifier abstraction we used for SMP
    syscall_work_queue inter_thread_wq;
    posix_thread _worker_thread;
    std::atomic<bool> _stopped = { false };
    std::atomic<bool> _main_thread_idle = { false };
    pthread_t _notify;
public:
    explicit thread_pool(sstring thread_name);
    ~thread_pool();
    template <typename T, typename Func>
    future<T> submit(Func func) {
        ++_aio_threaded_fallbacks;
        return inter_thread_wq.submit<T>(std::move(func));
    }
    uint64_t operation_count() const { return _aio_threaded_fallbacks; }

    unsigned complete() { return inter_thread_wq.complete(); }
    // Before we enter interrupt mode, we must make sure that the syscall thread will properly
    // generate signals to wake us up. This means we need to make sure that all modifications to
    // the pending and completed fields in the inter_thread_wq are visible to all threads.
    //
    // Simple release-acquire won't do because we also need to serialize all writes that happens
    // before the syscall thread loads this value, so we'll need full seq_cst.
    void enter_interrupt_mode() { _main_thread_idle.store(true, std::memory_order_seq_cst); }
    // When we exit interrupt mode, however, we can safely used relaxed order. If any reordering
    // takes place, we'll get an extra signal and complete will be called one extra time, which is
    // harmless.
    void exit_interrupt_mode() { _main_thread_idle.store(false, std::memory_order_relaxed); }

#else
public:
    template <typename T, typename Func>
    future<T> submit(Func func) { std::cout << "thread_pool not yet implemented on osv\n"; abort(); }
#endif
private:
    void work(sstring thread_name);
};

// The "reactor_backend" interface provides a method of waiting for various
// basic events on one thread. We have one implementation based on epoll and
// file-descriptors (reactor_backend_epoll) and one implementation based on
// OSv-specific file-descriptor-less mechanisms (reactor_backend_osv).
class reactor_backend {
public:
    virtual ~reactor_backend() {};
    // wait_and_process() waits for some events to become available, and
    // processes one or more of them. If block==false, it doesn't wait,
    // and just processes events that have already happened, if any.
    // After the optional wait, just before processing the events, the
    // pre_process() function is called.
    virtual bool wait_and_process(int timeout = -1, const sigset_t* active_sigmask = nullptr) = 0;
    // Methods that allow polling on file descriptors. This will only work on
    // reactor_backend_epoll. Other reactor_backend will probably abort if
    // they are called (which is fine if no file descriptors are waited on):
    virtual future<> readable(pollable_fd_state& fd) = 0;
    virtual future<> writeable(pollable_fd_state& fd) = 0;
    virtual void forget(pollable_fd_state& fd) = 0;
    // Methods that allow polling on a reactor_notifier. This is currently
    // used only for reactor_backend_osv, but in the future it should really
    // replace the above functions.
    virtual future<> notified(reactor_notifier *n) = 0;
    // Methods for allowing sending notifications events between threads.
    virtual std::unique_ptr<reactor_notifier> make_reactor_notifier() = 0;
};

// reactor backend using file-descriptor & epoll, suitable for running on
// Linux. Can wait on multiple file descriptors, and converts other events
// (such as timers, signals, inter-thread notifications) into file descriptors
// using mechanisms like timerfd, signalfd and eventfd respectively.
class reactor_backend_epoll : public reactor_backend {
private:
    file_desc _epollfd;
    future<> get_epoll_future(pollable_fd_state& fd,
            promise<> pollable_fd_state::* pr, int event);
    void complete_epoll_event(pollable_fd_state& fd,
            promise<> pollable_fd_state::* pr, int events, int event);
    void abort_fd(pollable_fd_state& fd, std::exception_ptr ex,
            promise<> pollable_fd_state::* pr, int event);
public:
    reactor_backend_epoll();
    virtual ~reactor_backend_epoll() override { }
    virtual bool wait_and_process(int timeout, const sigset_t* active_sigmask) override;
    virtual future<> readable(pollable_fd_state& fd) override;
    virtual future<> writeable(pollable_fd_state& fd) override;
    virtual void forget(pollable_fd_state& fd) override;
    virtual future<> notified(reactor_notifier *n) override;
    virtual std::unique_ptr<reactor_notifier> make_reactor_notifier() override;
    void abort_reader(pollable_fd_state& fd, std::exception_ptr ex);
    void abort_writer(pollable_fd_state& fd, std::exception_ptr ex);
};

#ifdef HAVE_OSV
// reactor_backend using OSv-specific features, without any file descriptors.
// This implementation cannot currently wait on file descriptors, but unlike
// reactor_backend_epoll it doesn't need file descriptors for waiting on a
// timer, for example, so file descriptors are not necessary.
class reactor_notifier_osv;
class reactor_backend_osv : public reactor_backend {
private:
    osv::newpoll::poller _poller;
    future<> get_poller_future(reactor_notifier_osv *n);
    promise<> _timer_promise;
public:
    reactor_backend_osv();
    virtual ~reactor_backend_osv() override { }
    virtual bool wait_and_process() override;
    virtual future<> readable(pollable_fd_state& fd) override;
    virtual future<> writeable(pollable_fd_state& fd) override;
    virtual void forget(pollable_fd_state& fd) override;
    virtual future<> notified(reactor_notifier *n) override;
    virtual std::unique_ptr<reactor_notifier> make_reactor_notifier() override;
    void enable_timer(steady_clock_type::time_point when);
    friend class reactor_notifier_osv;
};
#endif /* HAVE_OSV */

enum class open_flags {
    rw = O_RDWR,
    ro = O_RDONLY,
    wo = O_WRONLY,
    create = O_CREAT,
    truncate = O_TRUNC,
    exclusive = O_EXCL,
};

inline open_flags operator|(open_flags a, open_flags b) {
    return open_flags(static_cast<unsigned int>(a) | static_cast<unsigned int>(b));
}

class io_queue {
private:
    shard_id _coordinator;
    size_t _capacity;
    std::vector<shard_id> _io_topology;

    struct priority_class_data {
        priority_class_ptr ptr;
        size_t bytes;
        uint64_t ops;
        uint32_t nr_queued;
        std::chrono::duration<double> queue_time;
        metrics::metric_groups _metric_groups;
        priority_class_data(sstring name, priority_class_ptr ptr, shard_id owner);
    };

    std::unordered_map<unsigned, lw_shared_ptr<priority_class_data>> _priority_classes;
    fair_queue _fq;

    static constexpr unsigned _max_classes = 2048;
    static std::array<std::atomic<uint32_t>, _max_classes> _registered_shares;
    static std::array<sstring, _max_classes> _registered_names;

    static io_priority_class register_one_priority_class(sstring name, uint32_t shares);

    priority_class_data& find_or_create_class(const io_priority_class& pc, shard_id owner);
    static void fill_shares_array();
    friend smp;
public:

    io_queue(shard_id coordinator, size_t capacity, std::vector<shard_id> topology);
    ~io_queue();

    template <typename Func>
    static future<io_event>
    queue_request(shard_id coordinator, const io_priority_class& pc, size_t len, Func do_io);

    size_t capacity() const {
        return _capacity;
    }

    size_t queued_requests() const {
        return _fq.waiters();
    }

    shard_id coordinator() const {
        return _coordinator;
    }
    shard_id coordinator_of_shard(shard_id shard) const {
        return _io_topology[shard];
    }
    friend class reactor;
};

constexpr unsigned max_scheduling_groups() { return 16; }

class reactor {
    using sched_clock = std::chrono::steady_clock;
private:
    struct pollfn {
        virtual ~pollfn() {}
        // Returns true if work was done (false = idle)
        virtual bool poll() = 0;
        // Checks if work needs to be done, but without actually doing any
        // returns true if works needs to be done (false = idle)
        virtual bool pure_poll() = 0;
        // Tries to enter interrupt mode.
        //
        // If it returns true, then events from this poller will wake
        // a sleeping idle loop, and exit_interrupt_mode() must be called
        // to return to normal polling.
        //
        // If it returns false, the sleeping idle loop may not be entered.
        virtual bool try_enter_interrupt_mode() { return false; }
        virtual void exit_interrupt_mode() {}
    };
    struct task_queue;
    using task_queue_list = circular_buffer_fixed_capacity<task_queue*, max_scheduling_groups()>;

    class io_pollfn;
    class signal_pollfn;
    class aio_batch_submit_pollfn;
    class batch_flush_pollfn;
    class smp_pollfn;
    class drain_cross_cpu_freelist_pollfn;
    class lowres_timer_pollfn;
    class manual_timer_pollfn;
    class epoll_pollfn;
    class syscall_pollfn;
    class execution_stage_pollfn;
    friend io_pollfn;
    friend signal_pollfn;
    friend aio_batch_submit_pollfn;
    friend batch_flush_pollfn;
    friend smp_pollfn;
    friend drain_cross_cpu_freelist_pollfn;
    friend lowres_timer_pollfn;
    friend class manual_clock;
    friend class epoll_pollfn;
    friend class syscall_pollfn;
    friend class execution_stage_pollfn;
    friend class file_data_source_impl; // for fstream statistics
public:
    class poller {
        std::unique_ptr<pollfn> _pollfn;
        class registration_task;
        class deregistration_task;
        registration_task* _registration_task;
    public:
        template <typename Func> // signature: bool ()
        static poller simple(Func&& poll) {
            return poller(make_pollfn(std::forward<Func>(poll)));
        }
        poller(std::unique_ptr<pollfn> fn)
                : _pollfn(std::move(fn)) {
            do_register();
        }
        ~poller();
        poller(poller&& x);
        poller& operator=(poller&& x);
        void do_register();
        friend class reactor;
    };
    enum class idle_cpu_handler_result {
        no_more_work,
        interrupted_by_higher_priority_task
    };
    using work_waiting_on_reactor = const std::function<bool()>&;
    using idle_cpu_handler = std::function<idle_cpu_handler_result(work_waiting_on_reactor)>;

    struct io_stats {
        uint64_t aio_reads = 0;
        uint64_t aio_read_bytes = 0;
        uint64_t aio_writes = 0;
        uint64_t aio_write_bytes = 0;
        uint64_t fstream_reads = 0;
        uint64_t fstream_read_bytes = 0;
        uint64_t fstream_reads_blocked = 0;
        uint64_t fstream_read_bytes_blocked = 0;
        uint64_t fstream_read_aheads_discarded = 0;
        uint64_t fstream_read_ahead_discarded_bytes = 0;
    };
private:
    // FIXME: make _backend a unique_ptr<reactor_backend>, not a compile-time #ifdef.
#ifdef HAVE_OSV
    reactor_backend_osv _backend;
    sched::thread _timer_thread;
    sched::thread *_engine_thread;
    mutable mutex _timer_mutex;
    condvar _timer_cond;
    s64 _timer_due = 0;
#else
    reactor_backend_epoll _backend;
#endif
    sigset_t _active_sigmask; // holds sigmask while sleeping with sig disabled
    std::vector<pollfn*> _pollers;

    static constexpr size_t max_aio = 128;
    // Not all reactors have IO queues. If the number of IO queues is less than the number of shards,
    // some reactors will talk to foreign io_queues. If this reactor holds a valid IO queue, it will
    // be stored here.
    std::unique_ptr<io_queue> my_io_queue = {};


    // For submiting the actual IO, all we need is the coordinator id. So storing it
    // separately saves us the pointer access.
    shard_id _io_coordinator;
    io_queue* _io_queue;
    friend io_queue;

    std::vector<std::function<future<> ()>> _exit_funcs;
    unsigned _id = 0;
    bool _stopping = false;
    bool _stopped = false;
    condition_variable _stop_requested;
    bool _handle_sigint = true;
    promise<std::unique_ptr<network_stack>> _network_stack_ready_promise;
    int _return = 0;
    timer_t _steady_clock_timer = {};
    file_desc _task_quota_timer;
    promise<> _start_promise;
    semaphore _cpu_started;
    std::atomic<uint64_t> _tasks_processed = { 0 };
    std::atomic<uint64_t> _polls = { 0 };
    std::atomic<unsigned> _tasks_processed_stalled = { 0 };
    unsigned _tasks_processed_report_threshold;
    unsigned _stall_detector_reports_per_minute;
    std::atomic<uint64_t> _stall_detector_missed_ticks = { 0 };

    unsigned _max_task_backlog = 1000;
    timer_set<timer<>, &timer<>::_link> _timers;
    timer_set<timer<>, &timer<>::_link>::timer_list_t _expired_timers;
    timer_set<timer<lowres_clock>, &timer<lowres_clock>::_link> _lowres_timers;
    timer_set<timer<lowres_clock>, &timer<lowres_clock>::_link>::timer_list_t _expired_lowres_timers;
    timer_set<timer<manual_clock>, &timer<manual_clock>::_link> _manual_timers;
    timer_set<timer<manual_clock>, &timer<manual_clock>::_link>::timer_list_t _expired_manual_timers;
    io_context_t _io_context;
    std::vector<struct ::iocb> _pending_aio;
    semaphore _io_context_available;
    io_stats _io_stats;
    uint64_t _fsyncs = 0;
    uint64_t _cxx_exceptions = 0;
    struct task_queue {
        explicit task_queue(unsigned id, sstring name, float shares);
        int64_t _vruntime = 0;
        float _shares;
        int64_t _reciprocal_shares_times_2_power_32;
        bool _current = false;
        bool _active = false;
        uint8_t _id;
        sched_clock::duration _runtime = {};
        uint64_t _tasks_processed = 0;
        circular_buffer<std::unique_ptr<task>> _q;
        sstring _name;
        int64_t to_vruntime(sched_clock::duration runtime) const;
        void set_shares(float shares);
        struct indirect_compare;
        seastar::metrics::metric_groups _metrics;
    };
    boost::container::static_vector<std::unique_ptr<task_queue>, max_scheduling_groups()> _task_queues;
    int64_t _last_vruntime = 0;
    task_queue_list _active_task_queues;
    task_queue_list _activating_task_queues;
    task_queue* _at_destroy_tasks;
    sched_clock::duration _task_quota;
    /// Handler that will be called when there is no task to execute on cpu.
    /// It represents a low priority work.
    /// 
    /// Handler's return value determines whether handler did any actual work. If no work was done then reactor will go
    /// into sleep.
    ///
    /// Handler's argument is a function that returns true if a task which should be executed on cpu appears or false
    /// otherwise. This function should be used by a handler to return early if a task appears.
    idle_cpu_handler _idle_cpu_handler{ [] (work_waiting_on_reactor) {return idle_cpu_handler_result::no_more_work;} };
    std::unique_ptr<network_stack> _network_stack;
    // _lowres_clock_impl will only be created on cpu 0
    std::unique_ptr<lowres_clock_impl> _lowres_clock_impl;
    lowres_clock::time_point _lowres_next_timeout;
    std::experimental::optional<poller> _epoll_poller;
    std::experimental::optional<pollable_fd> _aio_eventfd;
    const bool _reuseport;
    circular_buffer<double> _loads;
    double _load = 0;
    sched_clock::duration _total_idle;
    sched_clock::time_point _start_time = sched_clock::now();
    std::chrono::nanoseconds _max_poll_time = calculate_poll_time();
    circular_buffer<output_stream<char>* > _flush_batching;
    std::atomic<bool> _sleeping alignas(seastar::cache_line_size);
    pthread_t _thread_id alignas(seastar::cache_line_size) = pthread_self();
    bool _strict_o_direct = true;
    bool _bypass_fsync = false;
    bool& _local_need_preempt{g_need_preempt}; // for access from the _task_quota_timer_thread
    std::thread _task_quota_timer_thread;
    std::atomic<bool> _dying{false};
private:
    static std::chrono::nanoseconds calculate_poll_time();
    static void block_notifier(int);
    void wakeup();
    bool flush_pending_aio();
    bool flush_tcp_batches();
    bool do_expire_lowres_timers();
    bool do_check_lowres_timers() const;
    void expire_manual_timers();
    void abort_on_error(int ret);
    void start_aio_eventfd_loop();
    void stop_aio_eventfd_loop();
    template <typename T, typename E, typename EnableFunc>
    void complete_timers(T&, E&, EnableFunc&& enable_fn);

    /**
     * Returns TRUE if all pollers allow blocking.
     *
     * @return FALSE if at least one of the blockers requires a non-blocking
     *         execution.
     */
    bool poll_once();
    bool pure_poll_once();
    template <typename Func> // signature: bool ()
    static std::unique_ptr<pollfn> make_pollfn(Func&& func);

    class signals {
    public:
        signals();
        ~signals();

        bool poll_signal();
        bool pure_poll_signal() const;
        void handle_signal(int signo, std::function<void ()>&& handler);
        void handle_signal_once(int signo, std::function<void ()>&& handler);
        static void action(int signo, siginfo_t* siginfo, void* ignore);

    private:
        struct signal_handler {
            signal_handler(int signo, std::function<void ()>&& handler);
            std::function<void ()> _handler;
        };
        std::atomic<uint64_t> _pending_signals;
        std::unordered_map<int, signal_handler> _signal_handlers;
    };

    signals _signals;
    thread_pool _thread_pool;
    friend class thread_pool;

    uint64_t pending_task_count() const;
    void run_tasks(task_queue& tq);
    bool have_more_tasks() const;
    bool posix_reuseport_detect();
    void task_quota_timer_thread_fn();
    void run_some_tasks(sched_clock::time_point& t_run_completed);
    void activate(task_queue& tq);
    void insert_active_task_queue(task_queue* tq);
    void insert_activating_task_queues();
    void account_runtime(task_queue& tq, sched_clock::duration runtime);
    void account_idle(sched_clock::duration idletime);
    void init_scheduling_group(scheduling_group sg, sstring name, float shares);
    uint64_t tasks_processed() const;
    uint64_t min_vruntime() const;
public:
    static boost::program_options::options_description get_options_description(std::chrono::duration<double> default_task_quota);
    explicit reactor(unsigned id);
    reactor(const reactor&) = delete;
    ~reactor();
    void operator=(const reactor&) = delete;

    const io_queue& get_io_queue() const {
        return *_io_queue;
    }

    io_priority_class register_one_priority_class(sstring name, uint32_t shares) {
        return io_queue::register_one_priority_class(std::move(name), shares);
    }

    void configure(boost::program_options::variables_map config);

    server_socket listen(socket_address sa, listen_options opts = {});

    future<connected_socket> connect(socket_address sa);
    future<connected_socket> connect(socket_address, socket_address, transport proto = transport::TCP);

    pollable_fd posix_listen(socket_address sa, listen_options opts = {});

    bool posix_reuseport_available() const { return _reuseport; }

    lw_shared_ptr<pollable_fd> make_pollable_fd(socket_address sa, transport proto = transport::TCP);
    future<> posix_connect(lw_shared_ptr<pollable_fd> pfd, socket_address sa, socket_address local);

    future<pollable_fd, socket_address> accept(pollable_fd_state& listen_fd);

    future<size_t> read_some(pollable_fd_state& fd, void* buffer, size_t size);
    future<size_t> read_some(pollable_fd_state& fd, const std::vector<iovec>& iov);

    future<size_t> write_some(pollable_fd_state& fd, const void* buffer, size_t size);

    future<> write_all(pollable_fd_state& fd, const void* buffer, size_t size);

    future<file> open_file_dma(sstring name, open_flags flags, file_open_options options = {});
    future<file> open_directory(sstring name);
    future<> make_directory(sstring name);
    future<> touch_directory(sstring name);
    future<std::experimental::optional<directory_entry_type>>  file_type(sstring name);
    future<uint64_t> file_size(sstring pathname);
    future<bool> file_exists(sstring pathname);
    future<fs_type> file_system_at(sstring pathname);
    future<> remove_file(sstring pathname);
    future<> rename_file(sstring old_pathname, sstring new_pathname);
    future<> link_file(sstring oldpath, sstring newpath);

    // In the following three methods, prepare_io is not guaranteed to execute in the same processor
    // in which it was generated. Therefore, care must be taken to avoid the use of objects that could
    // be destroyed within or at exit of prepare_io.
    template <typename Func>
    future<io_event> submit_io(Func prepare_io);
    template <typename Func>
    future<io_event> submit_io_read(const io_priority_class& priority_class, size_t len, Func prepare_io);
    template <typename Func>
    future<io_event> submit_io_write(const io_priority_class& priority_class, size_t len, Func prepare_io);

    int run();
    void exit(int ret);
    future<> when_started() { return _start_promise.get_future(); }
    // The function waits for timeout period for reactor stop notification
    // which happens on termination signals or call for exit().
    template <typename Rep, typename Period>
    future<> wait_for_stop(std::chrono::duration<Rep, Period> timeout) {
        return _stop_requested.wait(timeout, [this] { return _stopping; });
    }

    void at_exit(std::function<future<> ()> func);

    template <typename Func>
    void at_destroy(Func&& func) {
        _at_destroy_tasks->_q.push_back(make_task(default_scheduling_group(), std::forward<Func>(func)));
    }

    void add_task(std::unique_ptr<task>&& t) {
        auto sg = t->group();
        auto* q = _task_queues[sg._id].get();
        bool was_empty = q->_q.empty();
        q->_q.push_back(std::move(t));
        if (was_empty) {
            activate(*q);
        }
    }
    void add_urgent_task(std::unique_ptr<task>&& t) {
        auto sg = t->group();
        auto* q = _task_queues[sg._id].get();
        bool was_empty = q->_q.empty();
        q->_q.push_front(std::move(t));
        if (was_empty) {
            activate(*q);
        }
    }

    /// Set a handler that will be called when there is no task to execute on cpu.
    /// Handler should do a low priority work.
    /// 
    /// Handler's return value determines whether handler did any actual work. If no work was done then reactor will go
    /// into sleep.
    ///
    /// Handler's argument is a function that returns true if a task which should be executed on cpu appears or false
    /// otherwise. This function should be used by a handler to return early if a task appears.
    void set_idle_cpu_handler(idle_cpu_handler&& handler) {
        _idle_cpu_handler = std::move(handler);
    }
    void force_poll();

    void add_high_priority_task(std::unique_ptr<task>&&);

    network_stack& net() { return *_network_stack; }
    shard_id cpu_id() const { return _id; }

    void start_epoll();
    void sleep();

    steady_clock_type::duration total_idle_time();
    steady_clock_type::duration total_busy_time();

    const io_stats& get_io_stats() const { return _io_stats; }
#ifdef HAVE_OSV
    void timer_thread_func();
    void set_timer(sched::timer &tmr, s64 t);
#endif
private:
    /**
     * Add a new "poller" - a non-blocking function returning a boolean, that
     * will be called every iteration of a main loop.
     * If it returns FALSE then reactor's main loop is forbidden to block in the
     * current iteration.
     *
     * @param fn a new "poller" function to register
     */
    void register_poller(pollfn* p);
    void unregister_poller(pollfn* p);
    void replace_poller(pollfn* old, pollfn* neww);
    void register_metrics();
    future<> write_all_part(pollable_fd_state& fd, const void* buffer, size_t size, size_t completed);

    bool process_io();

    void add_timer(timer<steady_clock_type>*);
    bool queue_timer(timer<steady_clock_type>*);
    void del_timer(timer<steady_clock_type>*);
    void add_timer(timer<lowres_clock>*);
    bool queue_timer(timer<lowres_clock>*);
    void del_timer(timer<lowres_clock>*);
    void add_timer(timer<manual_clock>*);
    bool queue_timer(timer<manual_clock>*);
    void del_timer(timer<manual_clock>*);

    future<> run_exit_tasks();
    void stop();
    friend class pollable_fd;
    friend class pollable_fd_state;
    friend class posix_file_impl;
    friend class blockdev_file_impl;
    friend class readable_eventfd;
    friend class timer<>;
    friend class timer<lowres_clock>;
    friend class timer<manual_clock>;
    friend class smp;
    friend class smp_message_queue;
    friend class poller;
    friend class scheduling_group;
    friend void add_to_flush_poller(output_stream<char>* os);
    friend int ::_Unwind_RaiseException(void *h);
    metrics::metric_groups _metric_groups;
    friend future<scheduling_group> create_scheduling_group(sstring name, float shares);
public:
    bool wait_and_process(int timeout = 0, const sigset_t* active_sigmask = nullptr) {
        return _backend.wait_and_process(timeout, active_sigmask);
    }

    future<> readable(pollable_fd_state& fd) {
        return _backend.readable(fd);
    }
    future<> writeable(pollable_fd_state& fd) {
        return _backend.writeable(fd);
    }
    void forget(pollable_fd_state& fd) {
        _backend.forget(fd);
    }
    future<> notified(reactor_notifier *n) {
        return _backend.notified(n);
    }
    void abort_reader(pollable_fd_state& fd, std::exception_ptr ex) {
        return _backend.abort_reader(fd, std::move(ex));
    }
    void abort_writer(pollable_fd_state& fd, std::exception_ptr ex) {
        return _backend.abort_writer(fd, std::move(ex));
    }
    void enable_timer(steady_clock_type::time_point when);
    std::unique_ptr<reactor_notifier> make_reactor_notifier() {
        return _backend.make_reactor_notifier();
    }
    /// Sets the "Strict DMA" flag.
    ///
    /// When true (default), file I/O operations must use DMA.  This is
    /// the most performant option, but does not work on some file systems
    /// such as tmpfs or aufs (used in some Docker setups).
    ///
    /// When false, file I/O operations can fall back to buffered I/O if
    /// DMA is not available.  This can result in dramatic reducation in
    /// performance and an increase in memory consumption.
    void set_strict_dma(bool value) {
        _strict_o_direct = value;
    }
    void set_bypass_fsync(bool value) {
        _bypass_fsync = value;
    }
};

template <typename Func> // signature: bool ()
inline
std::unique_ptr<reactor::pollfn>
reactor::make_pollfn(Func&& func) {
    struct the_pollfn : pollfn {
        the_pollfn(Func&& func) : func(std::forward<Func>(func)) {}
        Func func;
        virtual bool poll() override final {
            return func();
        }
        virtual bool pure_poll() override final {
            return poll(); // dubious, but compatible
        }
    };
    return std::make_unique<the_pollfn>(std::forward<Func>(func));
}

extern __thread reactor* local_engine;
extern __thread size_t task_quota;

inline reactor& engine() {
    return *local_engine;
}

inline bool engine_is_ready() {
    return local_engine != nullptr;
}

class smp {
    static std::vector<posix_thread> _threads;
    static std::vector<std::function<void ()>> _thread_loops; // for dpdk
    static std::experimental::optional<boost::barrier> _all_event_loops_done;
    static std::vector<reactor*> _reactors;
    static smp_message_queue** _qs;
    static std::thread::id _tmain;
    static bool _using_dpdk;

    template <typename Func>
    using returns_future = is_future<std::result_of_t<Func()>>;
    template <typename Func>
    using returns_void = std::is_same<std::result_of_t<Func()>, void>;
public:
    static boost::program_options::options_description get_options_description();
    static void configure(boost::program_options::variables_map vm);
    static void cleanup();
    static void cleanup_cpu();
    static void arrive_at_event_loop_end();
    static void join_all();
    static bool main_thread() { return std::this_thread::get_id() == _tmain; }

    /// Runs a function on a remote core.
    ///
    /// \param t designates the core to run the function on (may be a remote
    ///          core or the local core).
    /// \param func a callable to run on core \c t.  If \c func is a temporary object,
    ///          its lifetime will be extended by moving it.  If @func is a reference,
    ///          the caller must guarantee that it will survive the call.
    /// \return whatever \c func returns, as a future<> (if \c func does not return a future,
    ///         submit_to() will wrap it in a future<>).
    template <typename Func>
    static futurize_t<std::result_of_t<Func()>> submit_to(unsigned t, Func&& func) {
        using ret_type = std::result_of_t<Func()>;
        if (t == engine().cpu_id()) {
            try {
                if (!is_future<ret_type>::value) {
                    // Non-deferring function, so don't worry about func lifetime
                    return futurize<ret_type>::apply(std::forward<Func>(func));
                } else if (std::is_lvalue_reference<Func>::value) {
                    // func is an lvalue, so caller worries about its lifetime
                    return futurize<ret_type>::apply(func);
                } else {
                    // Deferring call on rvalue function, make sure to preserve it across call
                    auto w = std::make_unique<std::decay_t<Func>>(std::move(func));
                    auto ret = futurize<ret_type>::apply(*w);
                    return ret.finally([w = std::move(w)] {});
                }
            } catch (...) {
                // Consistently return a failed future rather than throwing, to simplify callers
                return futurize<std::result_of_t<Func()>>::make_exception_future(std::current_exception());
            }
        } else {
            return _qs[t][engine().cpu_id()].submit(std::forward<Func>(func));
        }
    }
    static bool poll_queues();
    static bool pure_poll_queues();
    static boost::integer_range<unsigned> all_cpus() {
        return boost::irange(0u, count);
    }
    // Invokes func on all shards.
    // The returned future resolves when all async invocations finish.
    // The func may return void or future<>.
    // Each async invocation will work with a separate copy of func.
    template<typename Func>
    static future<> invoke_on_all(Func&& func) {
        static_assert(std::is_same<future<>, typename futurize<std::result_of_t<Func()>>::type>::value, "bad Func signature");
        return parallel_for_each(all_cpus(), [&func] (unsigned id) {
            return smp::submit_to(id, Func(func));
        });
    }
private:
    static void start_all_queues();
    static void pin(unsigned cpu_id);
    static void allocate_reactor(unsigned id);
    static void create_thread(std::function<void ()> thread_loop);
public:
    static unsigned count;
};

inline
pollable_fd_state::~pollable_fd_state() {
    engine().forget(*this);
}

inline
size_t iovec_len(const iovec* begin, size_t len)
{
    size_t ret = 0;
    auto end = begin + len;
    while (begin != end) {
        ret += begin++->iov_len;
    }
    return ret;
}

inline
future<pollable_fd, socket_address>
reactor::accept(pollable_fd_state& listenfd) {
    return readable(listenfd).then([&listenfd] () mutable {
        socket_address sa;
        socklen_t sl = sizeof(&sa.u.sas);
        file_desc fd = listenfd.fd.accept(sa.u.sa, sl, SOCK_NONBLOCK | SOCK_CLOEXEC);
        pollable_fd pfd(std::move(fd), pollable_fd::speculation(EPOLLOUT));
        return make_ready_future<pollable_fd, socket_address>(std::move(pfd), std::move(sa));
    });
}

inline
future<size_t>
reactor::read_some(pollable_fd_state& fd, void* buffer, size_t len) {
    return readable(fd).then([this, &fd, buffer, len] () mutable {
        auto r = fd.fd.read(buffer, len);
        if (!r) {
            return read_some(fd, buffer, len);
        }
        if (size_t(*r) == len) {
            fd.speculate_epoll(EPOLLIN);
        }
        return make_ready_future<size_t>(*r);
    });
}

inline
future<size_t>
reactor::read_some(pollable_fd_state& fd, const std::vector<iovec>& iov) {
    return readable(fd).then([this, &fd, iov = iov] () mutable {
        ::msghdr mh = {};
        mh.msg_iov = &iov[0];
        mh.msg_iovlen = iov.size();
        auto r = fd.fd.recvmsg(&mh, 0);
        if (!r) {
            return read_some(fd, iov);
        }
        if (size_t(*r) == iovec_len(iov)) {
            fd.speculate_epoll(EPOLLIN);
        }
        return make_ready_future<size_t>(*r);
    });
}

inline
future<size_t>
reactor::write_some(pollable_fd_state& fd, const void* buffer, size_t len) {
    return writeable(fd).then([this, &fd, buffer, len] () mutable {
        auto r = fd.fd.send(buffer, len, MSG_NOSIGNAL);
        if (!r) {
            return write_some(fd, buffer, len);
        }
        if (size_t(*r) == len) {
            fd.speculate_epoll(EPOLLOUT);
        }
        return make_ready_future<size_t>(*r);
    });
}

inline
future<>
reactor::write_all_part(pollable_fd_state& fd, const void* buffer, size_t len, size_t completed) {
    if (completed == len) {
        return make_ready_future<>();
    } else {
        return write_some(fd, static_cast<const char*>(buffer) + completed, len - completed).then(
                [&fd, buffer, len, completed, this] (size_t part) mutable {
            return write_all_part(fd, buffer, len, completed + part);
        });
    }
}

inline
future<>
reactor::write_all(pollable_fd_state& fd, const void* buffer, size_t len) {
    assert(len);
    return write_all_part(fd, buffer, len, 0);
}

inline
future<size_t> pollable_fd::read_some(char* buffer, size_t size) {
    return engine().read_some(*_s, buffer, size);
}

inline
future<size_t> pollable_fd::read_some(uint8_t* buffer, size_t size) {
    return engine().read_some(*_s, buffer, size);
}

inline
future<size_t> pollable_fd::read_some(const std::vector<iovec>& iov) {
    return engine().read_some(*_s, iov);
}

inline
future<> pollable_fd::write_all(const char* buffer, size_t size) {
    return engine().write_all(*_s, buffer, size);
}

inline
future<> pollable_fd::write_all(const uint8_t* buffer, size_t size) {
    return engine().write_all(*_s, buffer, size);
}

inline
future<size_t> pollable_fd::write_some(net::packet& p) {
    return engine().writeable(*_s).then([this, &p] () mutable {
        static_assert(offsetof(iovec, iov_base) == offsetof(net::fragment, base) &&
            sizeof(iovec::iov_base) == sizeof(net::fragment::base) &&
            offsetof(iovec, iov_len) == offsetof(net::fragment, size) &&
            sizeof(iovec::iov_len) == sizeof(net::fragment::size) &&
            alignof(iovec) == alignof(net::fragment) &&
            sizeof(iovec) == sizeof(net::fragment)
            , "net::fragment and iovec should be equivalent");

        iovec* iov = reinterpret_cast<iovec*>(p.fragment_array());
        msghdr mh = {};
        mh.msg_iov = iov;
        mh.msg_iovlen = p.nr_frags();
        auto r = get_file_desc().sendmsg(&mh, MSG_NOSIGNAL);
        if (!r) {
            return write_some(p);
        }
        if (size_t(*r) == p.len()) {
            _s->speculate_epoll(EPOLLOUT);
        }
        return make_ready_future<size_t>(*r);
    });
}

inline
future<> pollable_fd::write_all(net::packet& p) {
    return write_some(p).then([this, &p] (size_t size) {
        if (p.len() == size) {
            return make_ready_future<>();
        }
        p.trim_front(size);
        return write_all(p);
    });
}

inline
future<> pollable_fd::readable() {
    return engine().readable(*_s);
}

inline
future<> pollable_fd::writeable() {
    return engine().writeable(*_s);
}

inline
void
pollable_fd::abort_reader(std::exception_ptr ex) {
    engine().abort_reader(*_s, std::move(ex));
}

inline
void
pollable_fd::abort_writer(std::exception_ptr ex) {
    engine().abort_writer(*_s, std::move(ex));
}

inline
future<pollable_fd, socket_address> pollable_fd::accept() {
    return engine().accept(*_s);
}

inline
future<size_t> pollable_fd::recvmsg(struct msghdr *msg) {
    return engine().readable(*_s).then([this, msg] {
        auto r = get_file_desc().recvmsg(msg, 0);
        if (!r) {
            return recvmsg(msg);
        }
        // We always speculate here to optimize for throughput in a workload
        // with multiple outstanding requests. This way the caller can consume
        // all messages without resorting to epoll. However this adds extra
        // recvmsg() call when we hit the empty queue condition, so it may
        // hurt request-response workload in which the queue is empty when we
        // initially enter recvmsg(). If that turns out to be a problem, we can
        // improve speculation by using recvmmsg().
        _s->speculate_epoll(EPOLLIN);
        return make_ready_future<size_t>(*r);
    });
};

inline
future<size_t> pollable_fd::sendmsg(struct msghdr* msg) {
    return engine().writeable(*_s).then([this, msg] () mutable {
        auto r = get_file_desc().sendmsg(msg, 0);
        if (!r) {
            return sendmsg(msg);
        }
        // For UDP this will always speculate. We can't know if there's room
        // or not, but most of the time there should be so the cost of mis-
        // speculation is amortized.
        if (size_t(*r) == iovec_len(msg->msg_iov, msg->msg_iovlen)) {
            _s->speculate_epoll(EPOLLOUT);
        }
        return make_ready_future<size_t>(*r);
    });
}

inline
future<size_t> pollable_fd::sendto(socket_address addr, const void* buf, size_t len) {
    return engine().writeable(*_s).then([this, buf, len, addr] () mutable {
        auto r = get_file_desc().sendto(addr, buf, len, 0);
        if (!r) {
            return sendto(std::move(addr), buf, len);
        }
        // See the comment about speculation in sendmsg().
        if (size_t(*r) == len) {
            _s->speculate_epoll(EPOLLOUT);
        }
        return make_ready_future<size_t>(*r);
    });
}

template <typename Clock>
inline
timer<Clock>::timer(callback_t&& callback) : _callback(std::move(callback)) {
}

template <typename Clock>
inline
timer<Clock>::~timer() {
    if (_queued) {
        engine().del_timer(this);
    }
}

template <typename Clock>
inline
void timer<Clock>::set_callback(callback_t&& callback) {
    _callback = std::move(callback);
}

template <typename Clock>
inline
void timer<Clock>::arm_state(time_point until, std::experimental::optional<duration> period) {
    assert(!_armed);
    _period = period;
    _armed = true;
    _expired = false;
    _expiry = until;
    _queued = true;
}

template <typename Clock>
inline
void timer<Clock>::arm(time_point until, std::experimental::optional<duration> period) {
    arm_state(until, period);
    engine().add_timer(this);
}

template <typename Clock>
inline
void timer<Clock>::rearm(time_point until, std::experimental::optional<duration> period) {
    if (_armed) {
        cancel();
    }
    arm(until, period);
}

template <typename Clock>
inline
void timer<Clock>::arm(duration delta) {
    return arm(Clock::now() + delta);
}

template <typename Clock>
inline
void timer<Clock>::arm_periodic(duration delta) {
    arm(Clock::now() + delta, {delta});
}

template <typename Clock>
inline
void timer<Clock>::readd_periodic() {
    arm_state(Clock::now() + _period.value(), {_period.value()});
    engine().queue_timer(this);
}

template <typename Clock>
inline
bool timer<Clock>::cancel() {
    if (!_armed) {
        return false;
    }
    _armed = false;
    if (_queued) {
        engine().del_timer(this);
        _queued = false;
    }
    return true;
}

template <typename Clock>
inline
typename timer<Clock>::time_point timer<Clock>::get_timeout() {
    return _expiry;
}

extern logger seastar_logger;

}

#endif /* REACTOR_HH_ */
