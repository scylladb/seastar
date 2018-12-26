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

#define __user /* empty */  // for xfs includes, below

#include <cinttypes>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <sys/statfs.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <seastar/core/task.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/posix.hh>
#include <seastar/net/packet.hh>
#include <seastar/net/stack.hh>
#include <seastar/net/posix-stack.hh>
#include <seastar/net/native-stack.hh>
#include <seastar/core/resource.hh>
#include <seastar/core/print.hh>
#include "core/scollectd-impl.hh"
#include <seastar/util/conversions.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/systemwide_memory_barrier.hh>
#include <seastar/core/report_exception.hh>
#include <seastar/core/stall_sampler.hh>
#include <seastar/core/thread_cputime_clock.hh>
#include <seastar/util/log.hh>
#include "core/file-impl.hh"
#include <cassert>
#include <unistd.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <boost/lexical_cast.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/range/numeric.hpp>
#include <boost/range/algorithm/sort.hpp>
#include <boost/range/algorithm/remove_if.hpp>
#include <boost/algorithm/clamp.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/version.hpp>
#include <atomic>
#include <dirent.h>
#include <linux/types.h> // for xfs, below
#include <sys/ioctl.h>
#include <xfs/linux.h>
#define min min    /* prevent xfs.h from defining min() as a macro */
#include <xfs/xfs.h>
#undef min
#ifdef SEASTAR_HAVE_DPDK
#include <seastar/core/dpdk_rte.hh>
#include <rte_lcore.h>
#include <rte_launch.h>
#endif
#include <seastar/core/prefetch.hh>
#include <exception>
#include <regex>
#ifdef __GNUC__
#include <iostream>
#include <system_error>
#include <cxxabi.h>
#endif

#ifdef SEASTAR_SHUFFLE_TASK_QUEUE
#include <random>
#endif

#include <sys/mman.h>
#include <sys/utsname.h>
#include <linux/falloc.h>
#include <linux/magic.h>
#include <seastar/util/backtrace.hh>
#include <seastar/util/spinlock.hh>
#include <seastar/util/print_safe.hh>
#include <sys/sdt.h>

#ifdef HAVE_OSV
#include <osv/newpoll.hh>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#include <seastar/util/defer.hh>
#include <seastar/core/alien.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/execution_stage.hh>
#include <seastar/core/exception_hacks.hh>
#include "stall_detector.hh"

#include <yaml-cpp/yaml.h>

#ifdef SEASTAR_TASK_HISTOGRAM
#include <typeinfo>
#endif

namespace seastar {

struct mountpoint_params {
    std::string mountpoint = "none";
    uint64_t read_bytes_rate = std::numeric_limits<uint64_t>::max();
    uint64_t write_bytes_rate = std::numeric_limits<uint64_t>::max();
    uint64_t read_req_rate = std::numeric_limits<uint64_t>::max();
    uint64_t write_req_rate = std::numeric_limits<uint64_t>::max();
    uint64_t num_io_queues = 0; // calculated
};

}
    
namespace YAML {
template<>
struct convert<seastar::mountpoint_params> {
    static bool decode(const Node& node, seastar::mountpoint_params& mp) {
        using namespace seastar;
        mp.mountpoint = node["mountpoint"].as<std::string>().c_str();
        mp.read_bytes_rate = parse_memory_size(node["read_bandwidth"].as<std::string>());
        mp.read_req_rate = parse_memory_size(node["read_iops"].as<std::string>());
        mp.write_bytes_rate = parse_memory_size(node["write_bandwidth"].as<std::string>());
        mp.write_req_rate = parse_memory_size(node["write_iops"].as<std::string>());
        return true;
    }
};
}

namespace seastar {


namespace internal {

#ifdef SEASTAR_TASK_HISTOGRAM

class task_histogram {
    static constexpr unsigned max_countdown = 1'000'000;
    std::unordered_map<std::type_index, uint64_t> _histogram;
    unsigned _countdown_to_print = max_countdown;
public:
    void add(const task& t) {
        ++_histogram[std::type_index(typeid(t))];
        if (!--_countdown_to_print) {
            print();
            _countdown_to_print = max_countdown;
            _histogram.clear();
        }
    }
    void print() const {
        seastar::fmt::print("task histogram, {:d} task types {:d} tasks\n", _histogram.size(), max_countdown - _countdown_to_print);
        for (auto&& type_count : _histogram) {
            auto&& type = type_count.first;
            auto&& count = type_count.second;
            seastar::fmt::print("  {:10d} {}\n", count, type.name());
        }
    }
};

thread_local task_histogram this_thread_task_histogram;

#endif

void task_histogram_add_task(const task& t) {
#ifdef SEASTAR_TASK_HISTOGRAM
    this_thread_task_histogram.add(t);
#endif
}

}

using namespace std::chrono_literals;
namespace fs = seastar::compat::filesystem;

using namespace net;

using namespace internal;
using namespace internal::linux_abi;

seastar::logger seastar_logger("seastar");
seastar::logger sched_logger("scheduler");

std::atomic<lowres_clock_impl::steady_rep> lowres_clock_impl::counters::_steady_now;
std::atomic<lowres_clock_impl::system_rep> lowres_clock_impl::counters::_system_now;
std::atomic<manual_clock::rep> manual_clock::_now;
constexpr std::chrono::milliseconds lowres_clock_impl::_granularity;

constexpr unsigned reactor::max_queues;
constexpr unsigned reactor::max_aio_per_queue;

static bool sched_debug() {
    return false;
}

template <typename... Args>
void
sched_print(const char* fmt, Args&&... args) {
    if (sched_debug()) {
        sched_logger.trace(fmt, std::forward<Args>(args)...);
    }
}

timespec to_timespec(steady_clock_type::time_point t) {
    using ns = std::chrono::nanoseconds;
    auto n = std::chrono::duration_cast<ns>(t.time_since_epoch()).count();
    return { n / 1'000'000'000, n % 1'000'000'000 };
}

lowres_clock_impl::lowres_clock_impl() {
    update();
    _timer.set_callback(&lowres_clock_impl::update);
    _timer.arm_periodic(_granularity);
}

void lowres_clock_impl::update() {
    auto const steady_count =
            std::chrono::duration_cast<steady_duration>(base_steady_clock::now().time_since_epoch()).count();

    auto const system_count =
            std::chrono::duration_cast<system_duration>(base_system_clock::now().time_since_epoch()).count();

    counters::_steady_now.store(steady_count, std::memory_order_relaxed);
    counters::_system_now.store(system_count, std::memory_order_relaxed);
}

template <typename T>
struct syscall_result {
    T result;
    int error;
    syscall_result(T result, int error) : result{std::move(result)}, error{error} {
    }
    void throw_if_error() const {
        if (long(result) == -1) {
            throw std::system_error(ec());
        }
    }

    void throw_fs_exception(const sstring& reason, const fs::path& path) const {
        throw fs::filesystem_error(reason, path, ec());
    }

    void throw_fs_exception(const sstring& reason, const fs::path& path1, const fs::path& path2) const {
        throw fs::filesystem_error(reason, path1, path2, ec());
    }

    void throw_fs_exception_if_error(const sstring& reason, const sstring& path) const {
        if (long(result) == -1) {
            throw_fs_exception(reason, fs::path(path));
        }
    }

    void throw_fs_exception_if_error(const sstring& reason, const sstring& path1, const sstring& path2) const {
        if (long(result) == -1) {
            throw_fs_exception(reason, fs::path(path1), fs::path(path2));
        }
    }
protected:
    std::error_code ec() const {
        return std::error_code(error, std::system_category());
    }
};

// Wrapper for a system call result containing the return value,
// an output parameter that was returned from the syscall, and errno.
template <typename Extra>
struct syscall_result_extra : public syscall_result<int> {
    Extra extra;
    syscall_result_extra(int result, int error, Extra e) : syscall_result<int>{result, error}, extra{std::move(e)} {
    }
};

template <typename T>
syscall_result<T>
wrap_syscall(T result) {
    return syscall_result<T>{std::move(result), errno};
}

template <typename Extra>
syscall_result_extra<Extra>
wrap_syscall(int result, const Extra& extra) {
    return syscall_result_extra<Extra>{result, errno, extra};
}

inline int alarm_signal() {
    // We don't want to use SIGALRM, because the boost unit test library
    // also plays with it.
    return SIGRTMIN;
}

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
    virtual future<> readable_or_writeable(pollable_fd_state& fd) = 0;
    virtual void forget(pollable_fd_state& fd) = 0;
    // Calls reactor::signal_received(signo) when relevant
    virtual void handle_signal(int signo) = 0;
    virtual void start_tick() = 0;
    virtual void stop_tick() = 0;
    virtual void arm_highres_timer(const ::itimerspec& ts) = 0;
    virtual void reset_preemption_monitor() = 0;
    virtual void request_preemption() = 0;
};

// reactor backend using file-descriptor & epoll, suitable for running on
// Linux. Can wait on multiple file descriptors, and converts other events
// (such as timers, signals, inter-thread notifications) into file descriptors
// using mechanisms like timerfd, signalfd and eventfd respectively.
class reactor_backend_epoll : public reactor_backend {
    reactor* _r;
    std::thread _task_quota_timer_thread;
    timer_t _steady_clock_timer = {};
    bool _timer_enabled = false;
private:
    file_desc _epollfd;
    future<> get_epoll_future(pollable_fd_state& fd,
            promise<> pollable_fd_state::* pr, int event);
    void complete_epoll_event(pollable_fd_state& fd,
            promise<> pollable_fd_state::* pr, int events, int event);
    static void signal_received(int signo, siginfo_t* siginfo, void* ignore);
public:
    explicit reactor_backend_epoll(reactor* r);
    virtual ~reactor_backend_epoll() override;
    virtual bool wait_and_process(int timeout, const sigset_t* active_sigmask) override;
    virtual future<> readable(pollable_fd_state& fd) override;
    virtual future<> writeable(pollable_fd_state& fd) override;
    virtual future<> readable_or_writeable(pollable_fd_state& fd) override;
    virtual void forget(pollable_fd_state& fd) override;
    virtual void handle_signal(int signo) override;
    virtual void start_tick() override;
    virtual void stop_tick() override;
    virtual void arm_highres_timer(const ::itimerspec& ts) override;
    virtual void reset_preemption_monitor() override;
    virtual void request_preemption() override;
};

#ifdef HAVE_OSV
// reactor_backend using OSv-specific features, without any file descriptors.
// This implementation cannot currently wait on file descriptors, but unlike
// reactor_backend_epoll it doesn't need file descriptors for waiting on a
// timer, for example, so file descriptors are not necessary.
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
    void enable_timer(steady_clock_type::time_point when);
};
#endif /* HAVE_OSV */

class reactor_backend_aio : public reactor_backend {
    static constexpr size_t max_polls = 10000;
    reactor* _r;
    // We use two aio contexts, one for preempting events (the timer tick and
    // signals), the other for non-preempting events (fd poll).
    struct context {
        explicit context(size_t nr) : iocbs(new iocb*[nr]) {
            auto r = io_setup(nr, &io_context);
            throw_system_error_on(r == -1);
        }
        ~context() {
            io_destroy(io_context);
        }
        linux_abi::aio_context_t io_context{};
        std::unique_ptr<linux_abi::iocb*[]> iocbs;
        iocb** last = iocbs.get();
        void replenish(linux_abi::iocb* iocb, bool& flag) {
            if (!flag) {
                flag = true;
                queue(iocb);
            }
        }
        void queue(linux_abi::iocb* iocb) {
            *last++ = iocb;
        }
        void flush() {
            if (last != iocbs.get()) {
                auto nr = last - iocbs.get();
                last = iocbs.get();
                io_submit(io_context, nr, iocbs.get());
            }
        }
    };
    context _preempting_io{2}; // Used for the timer tick and the high resolution timer
    context _polling_io{max_polls}; // FIXME: unify with disk aio_context
    file_desc _steady_clock_timer = make_timerfd();
    linux_abi::iocb _task_quota_timer_iocb;
    linux_abi::iocb _timerfd_iocb;
    linux_abi::iocb _smp_wakeup_iocb;
    bool _task_quota_timer_in_preempting_io = false;
    bool _timerfd_in_preempting_io = false;
    bool _timerfd_in_polling_io = false;
    bool _smp_wakeup_in_polling_io = false;
    std::stack<std::unique_ptr<linux_abi::iocb>> _iocb_pool;
private:
    linux_abi::iocb* new_iocb() {
        if (_iocb_pool.empty()) {
            return new linux_abi::iocb;
        }
        auto ret = _iocb_pool.top().release();
        _iocb_pool.pop();
        return ret;
    }
    void free_iocb(linux_abi::iocb* iocb) {
        _iocb_pool.push(std::unique_ptr<linux_abi::iocb>(iocb));
    }
    static file_desc make_timerfd() {
        return file_desc::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC|TFD_NONBLOCK);
    }
    void process_task_quota_timer() {
        uint64_t v;
        (void)_r->_task_quota_timer.read(&v, 8);
    }
    void process_timerfd() {
        uint64_t expirations = 0;
        _steady_clock_timer.read(&expirations, 8);
        if (expirations) {
            _r->service_highres_timer();
        }
    }
    void process_smp_wakeup() {
        uint64_t ignore = 0;
        _r->_notify_eventfd.read(&ignore, 8);
    }
    bool service_preempting_io() {
        linux_abi::io_event a[2];
        auto r = io_getevents(_preempting_io.io_context, 0, 2, a, 0);
        assert(r != -1);
        bool did_work = false;
        for (unsigned i = 0; i != unsigned(r); ++i) {
            if (get_iocb(a[i]) == &_task_quota_timer_iocb) {
                _task_quota_timer_in_preempting_io = false;
                process_task_quota_timer();
            } else if (get_iocb(a[i]) == &_timerfd_iocb) {
                _timerfd_in_preempting_io = false;
                process_timerfd();
                did_work = true;
            }
        }
        return did_work;
    }
    bool await_events(int timeout, const sigset_t* active_sigmask) {
        ::timespec ts = {};
        ::timespec* tsp = [&] () -> ::timespec* {
            if (timeout == 0) {
                return &ts;
            } else if (timeout == -1) {
                return nullptr;
            } else {
                ts = posix::to_timespec(timeout * 1ms);
                return &ts;
            }
        }();
        constexpr size_t batch_size = 128;
        io_event batch[batch_size];
        bool did_work = false;
        int r;
        do {
            r = io_pgetevents(_polling_io.io_context, 1, batch_size, batch, tsp, active_sigmask);
            if (r == -1 && errno == EINTR) {
                return true;
            }
            assert(r != -1);
            for (unsigned i = 0; i != unsigned(r); ++i) {
                did_work = true;
                auto& event = batch[i];
                auto iocb = get_iocb(event);
                if (iocb == &_timerfd_iocb) {
                    _timerfd_in_polling_io = false;
                    process_timerfd();
                    continue;
                } else if (iocb == &_smp_wakeup_iocb) {
                    _smp_wakeup_in_polling_io = false;
                    process_smp_wakeup();
                    continue;
                }
                auto* pr = reinterpret_cast<promise<>*>(uintptr_t(event.data));
                pr->set_value();
                free_iocb(iocb);
            }
            // For the next iteration, don't use a timeout, since we may have waited already
            ts = {};
            tsp = &ts;
        } while (r == batch_size);
        return did_work;
    }
    static void signal_received(int signo, siginfo_t* siginfo, void* ignore) {
        engine()._signals.action(signo, siginfo, ignore);
    }
private:
    class io_poll_poller : public reactor::pollfn {
        reactor_backend_aio* _backend;
    public:
        explicit io_poll_poller(reactor_backend_aio* b) : _backend(b) {}
        virtual bool poll() override {
            return _backend->wait_and_process(0, nullptr);
        }
        virtual bool pure_poll() override {
            return _backend->wait_and_process(0, nullptr);
        }
        virtual bool try_enter_interrupt_mode() override {
            return true;
        }
        virtual void exit_interrupt_mode() override {}
    };
public:
    explicit reactor_backend_aio(reactor* r) : _r(r) {
        _task_quota_timer_iocb = make_poll_iocb(_r->_task_quota_timer.get(), POLLIN);
        _timerfd_iocb = make_poll_iocb(_steady_clock_timer.get(), POLLIN);
        _smp_wakeup_iocb = make_poll_iocb(_r->_notify_eventfd.get(), POLLIN);
        // Protect against spurious wakeups - if we get notified that the timer has
        // expired when it really hasn't, we don't want to block in read(tfd, ...).
        auto tfd = _r->_task_quota_timer.get();
        ::fcntl(tfd, F_SETFL, ::fcntl(tfd, F_GETFL) | O_NONBLOCK);
    }
    virtual bool wait_and_process(int timeout, const sigset_t* active_sigmask) override {
        bool did_work = service_preempting_io();
        if (did_work) {
            timeout = 0;
        }
        _polling_io.replenish(&_timerfd_iocb, _timerfd_in_polling_io);
        _polling_io.replenish(&_smp_wakeup_iocb, _smp_wakeup_in_polling_io);
        _polling_io.flush();
        if (timeout) {
            // If we get a signal during io_pgetevents(), its handler
            // will call request_preemption(), which needs the preemption monitor
            // to be armed:
            reset_preemption_monitor();
        }
        did_work |= await_events(timeout, active_sigmask);
        did_work |= service_preempting_io(); // clear task quota timer
        return did_work;
    }
    future<> poll(pollable_fd_state& fd, promise<> pollable_fd_state::*promise_field, int events) {
        if (!_r->_epoll_poller) {
            _r->_epoll_poller = reactor::poller(std::make_unique<io_poll_poller>(this));
        }
        try {
            if (events & fd.events_known) {
                fd.events_known &= ~events;
                return make_ready_future<>();
            }
            auto iocb = new_iocb(); // FIXME: merge with pollable_fd_state
            *iocb = make_poll_iocb(fd.fd.get(), events);
            fd.events_rw = events == (POLLIN|POLLOUT);
            auto pr = &(fd.*promise_field);
            *pr = promise<>();
            set_user_data(*iocb, pr);
            _polling_io.queue(iocb);
            return pr->get_future();
        } catch (...) {
            return make_exception_future<>(std::current_exception());
        }
    }
    virtual future<> readable(pollable_fd_state& fd) override {
        return poll(fd, &pollable_fd_state::pollin, POLLIN);
    }
    virtual future<> writeable(pollable_fd_state& fd) override {
        return poll(fd, &pollable_fd_state::pollout, POLLOUT);
    }
    virtual future<> readable_or_writeable(pollable_fd_state& fd) override {
        return poll(fd, &pollable_fd_state::pollin, POLLIN|POLLOUT);
    }
    virtual void forget(pollable_fd_state& fd) override {
        // ?
    }
    virtual void handle_signal(int signo) override {
        struct sigaction sa;
        sa.sa_sigaction = signal_received;
        sa.sa_mask = make_empty_sigset_mask();
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        auto r = ::sigaction(signo, &sa, nullptr);
        throw_system_error_on(r == -1);
        auto mask = make_sigset_mask(signo);
        r = ::pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
        throw_pthread_error(r);
    }
    virtual void start_tick() override {
        // Preempt whenever an event (timer tick or signal) is available on the
        // _preempting_io ring
        g_need_preempt = reinterpret_cast<const preemption_monitor*>(_preempting_io.io_context + 8);
        // reactor::request_preemption() will write to reactor::_preemption_monitor, which is now ignored
    }
    virtual void stop_tick() override {
        g_need_preempt = &_r->_preemption_monitor;
    }
    virtual void arm_highres_timer(const ::itimerspec& its) override {
        _steady_clock_timer.timerfd_settime(TFD_TIMER_ABSTIME, its);
    }
    virtual void reset_preemption_monitor() override {
        service_preempting_io();
        _preempting_io.replenish(&_timerfd_iocb, _timerfd_in_preempting_io);
        _preempting_io.replenish(&_task_quota_timer_iocb, _task_quota_timer_in_preempting_io);
        _preempting_io.flush();
    }
    virtual void request_preemption() override {
        ::itimerspec expired = {};
        expired.it_value.tv_nsec = 1;
        arm_highres_timer(expired); // will trigger immediately, triggering the preemption monitor
        // The kernel is not obliged to deliver the completion immediately, so wait for it
        while (!need_preempt()) {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    }
};

reactor_backend_epoll::reactor_backend_epoll(reactor* r)
        : _r(r), _epollfd(file_desc::epoll_create(EPOLL_CLOEXEC)) {
    ::epoll_event event;
    event.events = EPOLLIN;
    event.data.ptr = nullptr;
    auto ret = ::epoll_ctl(_epollfd.get(), EPOLL_CTL_ADD, _r->_notify_eventfd.get(), &event);
    throw_system_error_on(ret == -1);

    struct sigevent sev;
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev._sigev_un._tid = syscall(SYS_gettid);
    sev.sigev_signo = alarm_signal();
    ret = timer_create(CLOCK_MONOTONIC, &sev, &_steady_clock_timer);
    assert(ret >= 0);
}

reactor_backend_epoll::~reactor_backend_epoll() {
    timer_delete(_steady_clock_timer);
}

void reactor_backend_epoll::start_tick() {
    _task_quota_timer_thread = std::thread(&reactor::task_quota_timer_thread_fn, _r);

    ::sched_param sp;
    sp.sched_priority = 1;
    auto sched_ok = pthread_setschedparam(_task_quota_timer_thread.native_handle(), SCHED_FIFO, &sp);
    if (sched_ok != 0 && _r->_id == 0) {
        seastar_logger.warn("Unable to set SCHED_FIFO scheduling policy for timer thread; latency impact possible. Try adding CAP_SYS_NICE");
    }
}

void reactor_backend_epoll::stop_tick() {
    _r->_dying.store(true, std::memory_order_relaxed);
    _r->_task_quota_timer.timerfd_settime(0, seastar::posix::to_relative_itimerspec(1ns, 1ms)); // Make the timer fire soon
    _task_quota_timer_thread.join();
}

void reactor_backend_epoll::arm_highres_timer(const ::itimerspec& its) {
    auto ret = timer_settime(_steady_clock_timer, TIMER_ABSTIME, &its, NULL);
    throw_system_error_on(ret == -1);
    if (!_timer_enabled) {
        _timer_enabled = true;
        _r->_signals.handle_signal(alarm_signal(), [r = _r] {
            r->service_highres_timer();
        });
    }
}

reactor::signals::signals() : _pending_signals(0) {
}

reactor::signals::~signals() {
    sigset_t mask;
    sigfillset(&mask);
    ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
}

reactor::signals::signal_handler::signal_handler(int signo, std::function<void ()>&& handler)
        : _handler(std::move(handler)) {
    engine()._backend->handle_signal(signo);
}

void reactor_backend_epoll::handle_signal(int signo) {
    struct sigaction sa;
    sa.sa_sigaction = signal_received;
    sa.sa_mask = make_empty_sigset_mask();
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    auto r = ::sigaction(signo, &sa, nullptr);
    throw_system_error_on(r == -1);
    auto mask = make_sigset_mask(signo);
    r = ::pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
    throw_pthread_error(r);
}

void
reactor::signals::handle_signal(int signo, std::function<void ()>&& handler) {
    _signal_handlers.emplace(std::piecewise_construct,
        std::make_tuple(signo), std::make_tuple(signo, std::move(handler)));
}

void
reactor::signals::handle_signal_once(int signo, std::function<void ()>&& handler) {
    return handle_signal(signo, [fired = false, handler = std::move(handler)] () mutable {
        if (!fired) {
            fired = true;
            handler();
        }
    });
}

bool reactor::signals::poll_signal() {
    auto signals = _pending_signals.load(std::memory_order_relaxed);
    if (signals) {
        _pending_signals.fetch_and(~signals, std::memory_order_relaxed);
        for (size_t i = 0; i < sizeof(signals)*8; i++) {
            if (signals & (1ull << i)) {
               _signal_handlers.at(i)._handler();
            }
        }
    }
    return signals;
}

bool reactor::signals::pure_poll_signal() const {
    return _pending_signals.load(std::memory_order_relaxed);
}

void reactor::signals::action(int signo, siginfo_t* siginfo, void* ignore) {
    engine().request_preemption();
    engine()._signals._pending_signals.fetch_or(1ull << signo, std::memory_order_relaxed);
}

void reactor::signals::failed_to_handle(int signo) {
    char tname[64];
    pthread_getname_np(pthread_self(), tname, sizeof(tname));
    auto tid = syscall(SYS_gettid);
    seastar_logger.error("Failed to handle signal {} on thread {} ({}): engine not ready", signo, tid, tname);
}

void reactor::handle_signal(int signo, std::function<void ()>&& handler) {
    _signals.handle_signal(signo, std::move(handler));
}

void reactor_backend_epoll::signal_received(int signo, siginfo_t* siginfo, void* ignore) {
    if (engine_is_ready()) {
        engine()._signals.action(signo, siginfo, ignore);
    } else {
        reactor::signals::failed_to_handle(signo);
    }
}

// Accumulates an in-memory backtrace and flush to stderr eventually.
// Async-signal safe.
class backtrace_buffer {
    static constexpr unsigned _max_size = 8 << 10;
    unsigned _pos = 0;
    char _buf[_max_size];
public:
    void flush() noexcept {
        print_safe(_buf, _pos);
        _pos = 0;
    }

    void append(const char* str, size_t len) noexcept {
        if (_pos + len >= _max_size) {
            flush();
        }
        memcpy(_buf + _pos, str, len);
        _pos += len;
    }

    void append(const char* str) noexcept { append(str, strlen(str)); }

    template <typename Integral>
    void append_decimal(Integral n) noexcept {
        char buf[sizeof(n) * 3];
        auto len = convert_decimal_safe(buf, sizeof(buf), n);
        append(buf, len);
    }

    template <typename Integral>
    void append_hex(Integral ptr) noexcept {
        char buf[sizeof(ptr) * 2];
        convert_zero_padded_hex_safe(buf, sizeof(buf), ptr);
        append(buf, sizeof(buf));
    }

    void append_backtrace() noexcept {
        backtrace([this] (frame f) {
            append("  ");
            if (!f.so->name.empty()) {
                append(f.so->name.c_str(), f.so->name.size());
                append("+");
            }

            append("0x");
            append_hex(f.addr);
            append("\n");
        });
    }
};

static void print_with_backtrace(backtrace_buffer& buf) noexcept {
    if (local_engine) {
        buf.append(" on shard ");
        buf.append_decimal(local_engine->cpu_id());
    }

    buf.append(".\nBacktrace:\n");
    buf.append_backtrace();
    buf.flush();
}

static void print_with_backtrace(const char* cause) noexcept {
    backtrace_buffer buf;
    buf.append(cause);
    print_with_backtrace(buf);
}

// Installs signal handler stack for current thread.
// The stack remains installed as long as the returned object is kept alive.
// When it goes out of scope the previous handler is restored.
static decltype(auto) install_signal_handler_stack() {
    size_t size = SIGSTKSZ;
    auto mem = std::make_unique<char[]>(size);
    stack_t stack;
    stack_t prev_stack;
    stack.ss_sp = mem.get();
    stack.ss_flags = 0;
    stack.ss_size = size;
    auto r = sigaltstack(&stack, &prev_stack);
    throw_system_error_on(r == -1);
    return defer([mem = std::move(mem), prev_stack] () mutable {
        try {
            auto r = sigaltstack(&prev_stack, NULL);
            throw_system_error_on(r == -1);
        } catch (...) {
            mem.release(); // We failed to restore previous stack, must leak it.
            seastar_logger.error("Failed to restore signal stack: {}", std::current_exception());
        }
    });
}

reactor::task_queue::task_queue(unsigned id, sstring name, float shares)
        : _shares(std::max(shares, 1.0f))
        , _reciprocal_shares_times_2_power_32((uint64_t(1) << 32) / _shares)
        , _id(id)
        , _name(name) {
    namespace sm = seastar::metrics;
    static auto group = sm::label("group");
    auto group_label = group(_name);
    _metrics.add_group("scheduler", {
        sm::make_counter("runtime_ms", [this] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(_runtime).count();
        }, sm::description("Accumulated runtime of this task queue; an increment rate of 1000ms per second indicates full utilization"),
            {group_label}),
        sm::make_counter("tasks_processed", _tasks_processed,
                sm::description("Count of tasks executing on this queue; indicates together with runtime_ms indicates length of tasks"),
                {group_label}),
        sm::make_gauge("queue_length", [this] { return _q.size(); },
                sm::description("Size of backlog on this queue, in tasks; indicates whether the queue is busy and/or contended"),
                {group_label}),
        sm::make_gauge("shares", [this] { return _shares; },
                sm::description("Shares allocated to this queue"),
                {group_label}),
        sm::make_derive("time_spent_on_task_quota_violations_ms", [this] {
                return _time_spent_on_task_quota_violations / 1ms;
        }, sm::description("Total amount in milliseconds we were in violation of the task quota"),
           {group_label}),
    });
}

[[gnu::no_sanitize_undefined]]  // multiplication below may overflow; we check for that
inline
int64_t
reactor::task_queue::to_vruntime(sched_clock::duration runtime) const {
    auto scaled = (runtime.count() * _reciprocal_shares_times_2_power_32) >> 32;
    // Prevent overflow from returning ridiculous values
    return std::max<int64_t>(scaled, 0);
}

void
reactor::task_queue::set_shares(float shares) {
    _shares = std::max(shares, 1.0f);
    _reciprocal_shares_times_2_power_32 = (uint64_t(1) << 32) / _shares;
}

void
reactor::account_runtime(task_queue& tq, sched_clock::duration runtime) {
    if (runtime > _task_quota) {
        tq._time_spent_on_task_quota_violations += runtime - _task_quota;
    }
    tq._vruntime += tq.to_vruntime(runtime);
    tq._runtime += runtime;
}

void
reactor::account_idle(sched_clock::duration runtime) {
    // anything to do here?
}

struct reactor::task_queue::indirect_compare {
    bool operator()(const task_queue* tq1, const task_queue* tq2) const {
        return tq1->_vruntime < tq2->_vruntime;
    }
};

static bool detect_aio_poll() {
    auto fd = file_desc::eventfd(0, 0);
    aio_context_t ioc{};
    io_setup(1, &ioc);
    auto cleanup = defer([&] { io_destroy(ioc); });
    linux_abi::iocb iocb = internal::make_poll_iocb(fd.get(), POLLIN|POLLOUT);
    linux_abi::iocb* a[1] = { &iocb };
    auto r = io_submit(ioc, 1, a);
    if (r != 1) {
        return false;
    }
    uint64_t one = 1;
    fd.write(&one, 8);
    io_event ev[1];
    r = io_pgetevents(ioc, 1, 1, ev, nullptr, nullptr);
    return r == 1;
}

class reactor_backend_selector {
    std::string _name;
private:
    explicit reactor_backend_selector(std::string name) : _name(std::move(name)) {}
public:
    std::unique_ptr<reactor_backend> create(reactor* r) {
        if (_name == "linux-aio") {
            return std::make_unique<reactor_backend_aio>(r);
        } else if (_name == "epoll") {
            return std::make_unique<reactor_backend_epoll>(r);
        }
        throw std::logic_error("bad reactor backend");
    }
    static reactor_backend_selector default_backend() {
        return available()[0];
    }
    static std::vector<reactor_backend_selector> available() {
        std::vector<reactor_backend_selector> ret;
        if (detect_aio_poll()) {
            ret.push_back(reactor_backend_selector("linux-aio"));
        }
        ret.push_back(reactor_backend_selector("epoll"));
        return ret;
    }
    friend std::ostream& operator<<(std::ostream& os, const reactor_backend_selector& rbs) {
        return os << rbs._name;
    }
    friend void validate(boost::any& v, const std::vector<std::string> values, reactor_backend_selector* rbs, int) {
        namespace bpo = boost::program_options;
        bpo::validators::check_first_occurrence(v);
        auto s = bpo::validators::get_single_string(values);
        for (auto&& x : available()) {
            if (s == x._name) {
                v = std::move(x);
                return;
            }
        }
        throw bpo::validation_error(bpo::validation_error::invalid_option_value);
    }
};

reactor::reactor(unsigned id, reactor_backend_selector rbs)
    : _notify_eventfd(file_desc::eventfd(0, EFD_CLOEXEC))
    , _task_quota_timer(file_desc::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC))
    , _backend(rbs.create(this))
    , _id(id)
#ifdef HAVE_OSV
    , _timer_thread(
        [&] { timer_thread_func(); }, sched::thread::attr().stack(4096).name("timer_thread").pin(sched::cpu::current()))
    , _engine_thread(sched::thread::current())
#endif
    , _cpu_started(0)
    , _cpu_stall_detector(std::make_unique<cpu_stall_detector>(this))
    , _io_context(0)
    , _reuseport(posix_reuseport_detect())
    , _thread_pool(this, seastar::format("syscall-{}", id)) {
    _task_queues.push_back(std::make_unique<task_queue>(0, "main", 1000));
    _task_queues.push_back(std::make_unique<task_queue>(1, "atexit", 1000));
    _at_destroy_tasks = _task_queues.back().get();
    g_need_preempt = &_preemption_monitor;
    seastar::thread_impl::init();
    _backend->start_tick();
    for (unsigned i = 0; i != max_aio; ++i) {
        _free_iocbs.push(&_iocb_pool[i]);
    }
    auto r = io_setup(max_aio, &_io_context);
    assert(r >= 0);
#ifdef HAVE_OSV
    _timer_thread.start();
#else
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, alarm_signal());
    r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
    assert(r == 0);
    sigemptyset(&mask);
    sigaddset(&mask, cpu_stall_detector::signal_number());
    r = ::pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
    assert(r == 0);
#endif
    memory::set_reclaim_hook([this] (std::function<void ()> reclaim_fn) {
        add_high_priority_task(make_task(default_scheduling_group(), [fn = std::move(reclaim_fn)] {
            fn();
        }));
    });
}

reactor::~reactor() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, cpu_stall_detector::signal_number());
    auto r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
    assert(r == 0);

    _backend->stop_tick();
    auto eraser = [](auto& list) {
        while (!list.empty()) {
            auto& timer = *list.begin();
            timer.cancel();
        }
    };
    eraser(_expired_timers);
    eraser(_expired_lowres_timers);
    eraser(_expired_manual_timers);
    io_destroy(_io_context);
}

bool reactor::wait_and_process(int timeout, const sigset_t* active_sigmask) {
    return _backend->wait_and_process(timeout, active_sigmask);
}

future<> reactor::readable(pollable_fd_state& fd) {
    return _backend->readable(fd);
}

future<> reactor::writeable(pollable_fd_state& fd) {
    return _backend->writeable(fd);
}

future<> reactor::readable_or_writeable(pollable_fd_state& fd) {
    return _backend->readable_or_writeable(fd);
}

void reactor::forget(pollable_fd_state& fd) {
    _backend->forget(fd);
}

void reactor::abort_reader(pollable_fd_state& fd) {
    // TCP will respond to shutdown(SHUT_RD) by returning ECONNABORT on the next read,
    // but UDP responds by returning AGAIN. The no_more_recv flag tells us to convert
    // EAGAIN to ECONNABORT in that case.
    fd.no_more_recv = true;
    return fd.fd.shutdown(SHUT_RD);
}

void reactor::abort_writer(pollable_fd_state& fd) {
    // TCP will respond to shutdown(SHUT_WR) by returning ECONNABORT on the next write,
    // but UDP responds by returning AGAIN. The no_more_recv flag tells us to convert
    // EAGAIN to ECONNABORT in that case.
    fd.no_more_send = true;
    return fd.fd.shutdown(SHUT_WR);
}

void reactor::set_strict_dma(bool value) {
    _strict_o_direct = value;
}

void reactor::set_bypass_fsync(bool value) {
    _bypass_fsync = value;
}

void
reactor::reset_preemption_monitor() {
    return _backend->reset_preemption_monitor();
}

void reactor_backend_epoll::reset_preemption_monitor() {
    _r->_preemption_monitor.head.store(0, std::memory_order_relaxed);
}

void
reactor::request_preemption() {
    return _backend->request_preemption();
}

void
reactor_backend_epoll::request_preemption() {
    _r->_preemption_monitor.head.store(1, std::memory_order_relaxed);
}

// Add to an atomic integral non-atomically and returns the previous value
template <typename Integral>
inline Integral add_nonatomically(std::atomic<Integral>& value, Integral inc) {
    auto tmp = value.load(std::memory_order_relaxed);
    value.store(tmp + inc, std::memory_order_relaxed);
    return tmp;
}

// Increments an atomic integral non-atomically and returns the previous value
// Akin to value++;
template <typename Integral>
inline Integral increment_nonatomically(std::atomic<Integral>& value) {
    return add_nonatomically(value, Integral(1));
}

cpu_stall_detector::cpu_stall_detector(reactor* r, cpu_stall_detector_config cfg)
        : _r(r)
        , _shard_id(_r->cpu_id()) {
    update_config(cfg);
    struct sigevent sev = {};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = signal_number();
    sev._sigev_un._tid = syscall(SYS_gettid);
    int err = timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &_timer);
    if (err) {
        throw std::system_error(std::error_code(err, std::system_category()));
    }
    // note: if something is added here that can, it should take care to destroy _timer.
}

cpu_stall_detector::~cpu_stall_detector() {
    timer_delete(_timer);
}

cpu_stall_detector_config
cpu_stall_detector::get_config() const {
    return _config;
}

void cpu_stall_detector::update_config(cpu_stall_detector_config cfg) {
    _config = cfg;
    _threshold = std::chrono::duration_cast<std::chrono::steady_clock::duration>(cfg.threshold);
    _slack = std::chrono::duration_cast<std::chrono::steady_clock::duration>(cfg.threshold * cfg.slack);
    _stall_detector_reports_per_minute = cfg.stall_detector_reports_per_minute;
    _max_reports_per_minute = cfg.stall_detector_reports_per_minute;
    _rearm_timer_at = std::chrono::steady_clock::now();
}

void cpu_stall_detector::maybe_report() {
    if (_reported++ < _max_reports_per_minute) {
        generate_trace();
    }
}
// We use a tick at every timer firing so we can report suppressed backtraces.
// Best case it's a correctly predicted branch. If a backtrace had happened in
// the near past it's an increment and two branches.
//
// We can do it a cheaper if we don't report suppressed backtraces.
void cpu_stall_detector::on_signal() {
    if (_active.load(std::memory_order_relaxed)) {
        maybe_report();
        _report_at <<= 1;
        arm_timer();
    }
}

void cpu_stall_detector::report_suppressions(std::chrono::steady_clock::time_point now) {
    if (now > _minute_mark + 60s) {
        if (_reported) {
            auto supressed = _reported - _max_reports_per_minute;
            backtrace_buffer buf;
            // Reuse backtrace buffer infrastructure so we don't have to allocate here
            buf.append("Rate-limit: supressed ");
            buf.append_decimal(_reported - _max_reports_per_minute);
            supressed == 1 ? buf.append(" backtrace") : buf.append(" backtraces");
            buf.append(" on shard ");
            buf.append_decimal(_shard_id);
            buf.append("\n");
            buf.flush();
        }
        _reported = 0;
        _minute_mark = now;
    }
}

void cpu_stall_detector::arm_timer() {
    auto its = posix::to_relative_itimerspec(_threshold * _report_at + _slack, 0s);
    timer_settime(_timer, 0, &its, nullptr);
}

void cpu_stall_detector::start_task_run(std::chrono::steady_clock::time_point now) {
    if (now > _rearm_timer_at) {
        report_suppressions(now);
        _report_at = 1;
        _run_started_at = now;
        _rearm_timer_at = now + _threshold * _report_at;
        arm_timer();
    }
    _active.store(true, std::memory_order_relaxed);
    std::atomic_signal_fence(std::memory_order_release); // Don't delay this write, so the signal handler can see it
}

void cpu_stall_detector::end_task_run(std::chrono::steady_clock::time_point now) {
    std::atomic_signal_fence(std::memory_order_acquire); // Don't hoist this write, so the signal handler can see it
    _active.store(false, std::memory_order_relaxed);
}

void cpu_stall_detector::start_sleep() {
    auto its = posix::to_relative_itimerspec(0s,  0s);
    timer_settime(_timer, 0, &its, nullptr);
    _rearm_timer_at = std::chrono::steady_clock::now();
}

void cpu_stall_detector::end_sleep() {
}

void
reactor::task_quota_timer_thread_fn() {
    auto thread_name = seastar::format("timer-{}", _id);
    pthread_setname_np(pthread_self(), thread_name.c_str());

    sigset_t mask;
    sigfillset(&mask);            
    for (auto sig : { SIGSEGV }) {
        sigdelset(&mask, sig);
    }
    auto r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
    if (r) {
        seastar_logger.info("Thread {}: failed to block signals. Aborting.", thread_name.c_str());
        abort();
    }

    // We need to wait until task quota is set before we can calculate how many ticks are to
    // a minute. Technically task_quota is used from many threads, but since it is read-only here
    // and only used during initialization we will avoid complicating the code.
    {
        uint64_t events;
        _task_quota_timer.read(&events, 8);
        request_preemption();
    }

    while (!_dying.load(std::memory_order_relaxed)) {
        uint64_t events;
        _task_quota_timer.read(&events, 8);
        request_preemption();

        // We're in a different thread, but guaranteed to be on the same core, so even
        // a signal fence is overdoing it
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
}
void 
reactor::update_blocked_reactor_notify_ms(std::chrono::milliseconds ms) {
    auto cfg = _cpu_stall_detector->get_config();
    if (ms != cfg.threshold) {
        cfg.threshold = ms;
        _cpu_stall_detector->update_config(cfg);
        seastar_logger.info("updated: blocked-reactor-notify-ms={}", ms.count());
    }
}

std::chrono::milliseconds
reactor::get_blocked_reactor_notify_ms() const {
    auto d = _cpu_stall_detector->get_config().threshold;
    return std::chrono::duration_cast<std::chrono::milliseconds>(d);
}

void
reactor::set_stall_detector_report_function(std::function<void ()> report) {
    auto cfg = _cpu_stall_detector->get_config();
    cfg.report = std::move(report);
    _cpu_stall_detector->update_config(std::move(cfg));
}

std::function<void ()>
reactor::get_stall_detector_report_function() const {
    return _cpu_stall_detector->get_config().report;
}

void
reactor::block_notifier(int) {
    engine()._cpu_stall_detector->generate_trace();
}

void
cpu_stall_detector::generate_trace() {
    auto delta = std::chrono::steady_clock::now() - _run_started_at;

    if (_config.report) {
        _config.report();
        return;
    }

    backtrace_buffer buf;
    buf.append("Reactor stalled for ");
    buf.append_decimal(uint64_t(delta / 1ms));
    buf.append(" ms");
    print_with_backtrace(buf);
}

template <typename T, typename E, typename EnableFunc>
void reactor::complete_timers(T& timers, E& expired_timers, EnableFunc&& enable_fn) {
    expired_timers = timers.expire(timers.now());
    for (auto& t : expired_timers) {
        t._expired = true;
    }
    while (!expired_timers.empty()) {
        auto t = &*expired_timers.begin();
        expired_timers.pop_front();
        t->_queued = false;
        if (t->_armed) {
            t->_armed = false;
            if (t->_period) {
                t->readd_periodic();
            }
            try {
                t->_callback();
            } catch (...) {
                seastar_logger.error("Timer callback failed: {}", std::current_exception());
            }
        }
    }
    enable_fn();
}

#ifdef HAVE_OSV
void reactor::timer_thread_func() {
    sched::timer tmr(*sched::thread::current());
    WITH_LOCK(_timer_mutex) {
        while (!_stopped) {
            if (_timer_due != 0) {
                set_timer(tmr, _timer_due);
                _timer_cond.wait(_timer_mutex, &tmr);
                if (tmr.expired()) {
                    _timer_due = 0;
                    _engine_thread->unsafe_stop();
                    _pending_tasks.push_front(make_task(default_scheduling_group(), [this] {
                        complete_timers(_timers, _expired_timers, [this] {
                            if (!_timers.empty()) {
                                enable_timer(_timers.get_next_timeout());
                            }
                        });
                    }));
                    _engine_thread->wake();
                } else {
                    tmr.cancel();
                }
            } else {
                _timer_cond.wait(_timer_mutex);
            }
        }
    }
}

void reactor::set_timer(sched::timer &tmr, s64 t) {
    using namespace osv::clock;
    tmr.set(wall::time_point(std::chrono::nanoseconds(t)));
}
#endif

class network_stack_registry {
public:
    using options = boost::program_options::variables_map;
private:
    static std::unordered_map<sstring,
            std::function<future<std::unique_ptr<network_stack>> (options opts)>>& _map() {
        static std::unordered_map<sstring,
                std::function<future<std::unique_ptr<network_stack>> (options opts)>> map;
        return map;
    }
    static sstring& _default() {
        static sstring def;
        return def;
    }
public:
    static boost::program_options::options_description& options_description() {
        static boost::program_options::options_description opts;
        return opts;
    }
    static void register_stack(sstring name, boost::program_options::options_description opts,
        std::function<future<std::unique_ptr<network_stack>>(options opts)> create,
        bool make_default);
    static sstring default_stack();
    static std::vector<sstring> list();
    static future<std::unique_ptr<network_stack>> create(options opts);
    static future<std::unique_ptr<network_stack>> create(sstring name, options opts);
};

void reactor::configure(boost::program_options::variables_map vm) {
    auto network_stack_ready = vm.count("network-stack")
        ? network_stack_registry::create(sstring(vm["network-stack"].as<std::string>()), vm)
        : network_stack_registry::create(vm);
    network_stack_ready.then([this] (std::unique_ptr<network_stack> stack) {
        _network_stack_ready_promise.set_value(std::move(stack));
    });

    _handle_sigint = !vm.count("no-handle-interrupt");
    auto task_quota = vm["task-quota-ms"].as<double>() * 1ms;
    _task_quota = std::chrono::duration_cast<sched_clock::duration>(task_quota);

    auto blocked_time = vm["blocked-reactor-notify-ms"].as<unsigned>() * 1ms;
    cpu_stall_detector_config csdc;
    csdc.threshold = blocked_time;
    csdc.stall_detector_reports_per_minute = vm["blocked-reactor-reports-per-minute"].as<unsigned>();
    _cpu_stall_detector->update_config(csdc);

    _max_task_backlog = vm["max-task-backlog"].as<unsigned>();
    _max_poll_time = vm["idle-poll-time-us"].as<unsigned>() * 1us;
    if (vm.count("poll-mode")) {
        _max_poll_time = std::chrono::nanoseconds::max();
    }
    if (vm.count("overprovisioned")
           && vm["idle-poll-time-us"].defaulted()
           && !vm.count("poll-mode")) {
        _max_poll_time = 0us;
    }
    set_strict_dma(!vm.count("relaxed-dma"));
    if (!vm["poll-aio"].as<bool>()
            || (vm["poll-aio"].defaulted() && vm.count("overprovisioned"))) {
        _aio_eventfd = pollable_fd(file_desc::eventfd(0, 0));
    }
    set_bypass_fsync(vm["unsafe-bypass-fsync"].as<bool>());
    _force_io_getevents_syscall = vm["force-aio-syscalls"].as<bool>();
}

future<> reactor_backend_epoll::get_epoll_future(pollable_fd_state& pfd,
        promise<> pollable_fd_state::*pr, int event) {
    if (pfd.events_known & event) {
        pfd.events_known &= ~event;
        return make_ready_future();
    }
    pfd.events_rw = event == (EPOLLIN | EPOLLOUT);
    pfd.events_requested |= event;
    if ((pfd.events_epoll & event) != event) {
        auto ctl = pfd.events_epoll ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        pfd.events_epoll |= event;
        ::epoll_event eevt;
        eevt.events = pfd.events_epoll;
        eevt.data.ptr = &pfd;
        int r = ::epoll_ctl(_epollfd.get(), ctl, pfd.fd.get(), &eevt);
        assert(r == 0);
        engine().start_epoll();
    }
    pfd.*pr = promise<>();
    return (pfd.*pr).get_future();
}

future<> reactor_backend_epoll::readable(pollable_fd_state& fd) {
    return get_epoll_future(fd, &pollable_fd_state::pollin, EPOLLIN);
}

future<> reactor_backend_epoll::writeable(pollable_fd_state& fd) {
    return get_epoll_future(fd, &pollable_fd_state::pollout, EPOLLOUT);
}

future<> reactor_backend_epoll::readable_or_writeable(pollable_fd_state& fd) {
    return get_epoll_future(fd, &pollable_fd_state::pollin, EPOLLIN | EPOLLOUT);
}

void reactor_backend_epoll::forget(pollable_fd_state& fd) {
    if (fd.events_epoll) {
        ::epoll_ctl(_epollfd.get(), EPOLL_CTL_DEL, fd.fd.get(), nullptr);
    }
}

pollable_fd
reactor::posix_listen(socket_address sa, listen_options opts) {
    file_desc fd = file_desc::socket(sa.u.sa.sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, int(opts.proto));
    if (opts.reuse_address) {
        fd.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1);
    }
    if (_reuseport)
        fd.setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);

    fd.bind(sa.u.sa, sizeof(sa.u.sas));
    fd.listen(100);
    return pollable_fd(std::move(fd));
}

bool
reactor::posix_reuseport_detect() {
    return false; // FIXME: reuseport currently leads to heavy load imbalance. Until we fix that, just
                  // disable it unconditionally.
    try {
        file_desc fd = file_desc::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        fd.setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);
        return true;
    } catch(std::system_error& e) {
        return false;
    }
}

void pollable_fd::maybe_no_more_recv() {
    if (_s->no_more_recv) {
        throw std::system_error(std::error_code(ECONNABORTED, std::system_category()));
    }
}

void pollable_fd::maybe_no_more_send() {
    if (_s->no_more_send) {
        throw std::system_error(std::error_code(ECONNABORTED, std::system_category()));
    }
}

lw_shared_ptr<pollable_fd>
reactor::make_pollable_fd(socket_address sa, transport proto) {
    file_desc fd = file_desc::socket(sa.u.sa.sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, int(proto));
    return make_lw_shared<pollable_fd>(pollable_fd(std::move(fd)));
}

future<>
reactor::posix_connect(lw_shared_ptr<pollable_fd> pfd, socket_address sa, socket_address local) {
    pfd->get_file_desc().bind(local.u.sa, sizeof(sa.u.sas));
    pfd->get_file_desc().connect(sa.u.sa, sizeof(sa.u.sas));
    return pfd->writeable().then([pfd]() mutable {
        auto err = pfd->get_file_desc().getsockopt<int>(SOL_SOCKET, SO_ERROR);
        if (err != 0) {
            throw std::system_error(err, std::system_category());
        }
        return make_ready_future<>();
    });
}

server_socket
reactor::listen(socket_address sa, listen_options opt) {
    return server_socket(_network_stack->listen(sa, opt));
}

future<connected_socket>
reactor::connect(socket_address sa) {
    return _network_stack->connect(sa);
}

future<connected_socket>
reactor::connect(socket_address sa, socket_address local, transport proto) {
    return _network_stack->connect(sa, local, proto);
}

void reactor_backend_epoll::complete_epoll_event(pollable_fd_state& pfd, promise<> pollable_fd_state::*pr,
        int events, int event) {
    if (pfd.events_requested & events & event) {
        pfd.events_requested &= ~event;
        pfd.events_known &= ~event;
        (pfd.*pr).set_value();
        pfd.*pr = promise<>();
    }
}

// due to a kernel bug, fix pending: https://lore.kernel.org/lkml/9bab0f40-5748-f147-efeb-5aac4fd44533@scylladb.com/
static bool aio_nowait_supported = false;

class io_desc {
    promise<io_event> _pr;
    io_queue* _ioq_ptr;
    fair_queue_request_descriptor _fq_desc;
public:
    io_desc(io_queue* ioq, unsigned weight, unsigned size)
        : _ioq_ptr(ioq)
        , _fq_desc(fair_queue_request_descriptor{weight, size})
    {}

    fair_queue_request_descriptor& fq_descriptor() {
        return _fq_desc;
    }

    void notify_requests_finished() {
        _ioq_ptr->notify_requests_finished(_fq_desc);
    }

    void set_exception(std::exception_ptr eptr) {
        _pr.set_exception(std::move(eptr));
    }

    void set_value(io_event& ev) {
        _pr.set_value(ev);
    }

    future<io_event> get_future() {
        return _pr.get_future();
    }
};

template <typename Func>
void
reactor::submit_io(io_desc* desc, Func prepare_io) {
    iocb& io = *_free_iocbs.top();
    _free_iocbs.pop();
    prepare_io(io);
    if (_aio_eventfd) {
        set_eventfd_notification(io, _aio_eventfd->get_fd());
    }
    if (aio_nowait_supported) {
        set_nowait(io, true);
    }
    set_user_data(io, desc);
    _pending_aio.push_back(&io);
}

// Returns: number of iocbs consumed (0 or 1)
size_t
reactor::handle_aio_error(linux_abi::iocb* iocb, int ec) {
    switch (ec) {
        case EINVAL:
        case EOPNOTSUPP:
            aio_nowait_supported = false;
            set_nowait(*iocb, false);
            return 0;
        case EAGAIN:
            return 0;
        case EBADF: {
            auto desc = reinterpret_cast<io_desc*>(get_user_data(*iocb));
            _free_iocbs.push(iocb);
            try {
                throw std::system_error(EBADF, std::system_category());
            } catch (...) {
                desc->set_exception(std::current_exception());
            }
            desc->notify_requests_finished();
            delete desc;
            // if EBADF, it means that the first request has a bad fd, so
            // we will only remove it from _pending_aio and try again.
            return 1;
        }
        default:
            ++_io_stats.aio_errors;
            throw_system_error_on(true, "io_submit");
            abort();
    }
}

bool
reactor::flush_pending_aio() {
    for (auto& ioq : my_io_queues) {
        ioq->poll_io_queue();
    }

    bool did_work = false;
    while (!_pending_aio.empty()) {
        auto nr = _pending_aio.size();
        auto iocbs = _pending_aio.data();
        auto r = io_submit(_io_context, nr, iocbs);
        size_t nr_consumed;
        if (r == -1) {
            nr_consumed = handle_aio_error(iocbs[0], errno);
        } else {
            nr_consumed = size_t(r);
        }

        did_work = true;
        if (nr_consumed == nr) {
            _pending_aio.clear();
        } else {
            _pending_aio.erase(_pending_aio.begin(), _pending_aio.begin() + nr_consumed);
        }
    }
    if (!_pending_aio_retry.empty()) {
        auto retries = std::exchange(_pending_aio_retry, {});
        _thread_pool.submit<syscall_result<int>>([this, retries] () mutable {
            auto r = io_submit(_io_context, retries.size(), retries.data());
            return wrap_syscall<int>(r);
        }).then([this, retries] (syscall_result<int> result) {
            auto iocbs = retries.data();
            size_t nr_consumed = 0;
            if (result.result == -1) {
                nr_consumed = handle_aio_error(iocbs[0], result.error);
            } else {
                nr_consumed = result.result;
            }
            std::copy(retries.begin() + nr_consumed, retries.end(), std::back_inserter(_pending_aio_retry));
        });
        did_work = true;
    }
    return did_work;
}

const io_priority_class& default_priority_class() {
    static thread_local auto shard_default_class = [] {
        return engine().register_one_priority_class("default", 1);
    }();
    return shard_default_class;
}

template <typename Func>
future<io_event>
reactor::submit_io_read(io_queue* ioq, const io_priority_class& pc, size_t len, Func prepare_io) {
    ++_io_stats.aio_reads;
    _io_stats.aio_read_bytes += len;
    return ioq->queue_request(pc, len, io_queue::request_type::read, std::move(prepare_io));
}

template <typename Func>
future<io_event>
reactor::submit_io_write(io_queue* ioq, const io_priority_class& pc, size_t len, Func prepare_io) {
    ++_io_stats.aio_writes;
    _io_stats.aio_write_bytes += len;
    return ioq->queue_request(pc, len, io_queue::request_type::write, std::move(prepare_io));
}

bool reactor::process_io()
{
    io_event ev[max_aio];
    struct timespec timeout = {0, 0};
    auto n = io_getevents(_io_context, 1, max_aio, ev, &timeout, _force_io_getevents_syscall);
    if (n == -1 && errno == EINTR) {
        n = 0;
    }
    assert(n >= 0);
    unsigned nr_retry = 0;
    for (size_t i = 0; i < size_t(n); ++i) {
        auto iocb = get_iocb(ev[i]);
        if (ev[i].res == -EAGAIN) {
            ++nr_retry;
            set_nowait(*iocb, false);
            _pending_aio_retry.push_back(iocb);
            continue;
        }
        _free_iocbs.push(iocb);
        auto desc = reinterpret_cast<io_desc*>(ev[i].data);
        desc->set_value(ev[i]);
        desc->notify_requests_finished();
        delete desc;
    }
    return n;
}

fair_queue::config io_queue::make_fair_queue_config(config iocfg) {
    fair_queue::config cfg;
    cfg.capacity = std::min(iocfg.capacity, reactor::max_aio_per_queue);
    cfg.max_req_count = iocfg.max_req_count;
    cfg.max_bytes_count = iocfg.max_bytes_count;
    return cfg;
}

io_queue::io_queue(io_queue::config cfg)
    : _priority_classes()
    , _fq(make_fair_queue_config(cfg))
    , _config(std::move(cfg)) {
}

io_queue::~io_queue() {
    // It is illegal to stop the I/O queue with pending requests.
    // Technically we would use a gate to guarantee that. But here, it is not
    // needed since this is expected to be destroyed only after the reactor is destroyed.
    //
    // And that will happen only when there are no more fibers to run. If we ever change
    // that, then this has to change.
    for (auto&& pclasses: _priority_classes) {
        _fq.unregister_priority_class(pclasses.second->ptr);
    }
}

std::array<std::atomic<uint32_t>, io_queue::_max_classes> io_queue::_registered_shares;
// We could very well just add the name to the io_priority_class. However, because that
// structure is passed along all the time - and sometimes we can't help but copy it, better keep
// it lean. The name won't really be used for anything other than monitoring.
std::array<sstring, io_queue::_max_classes> io_queue::_registered_names;

void io_queue::fill_shares_array() {
    for (unsigned i = 0; i < _max_classes; ++i) {
        _registered_shares[i].store(0);
    }
}

io_priority_class io_queue::register_one_priority_class(sstring name, uint32_t shares) {
    for (unsigned i = 0; i < _max_classes; ++i) {
        uint32_t unused = 0;
        auto s = _registered_shares[i].compare_exchange_strong(unused, shares, std::memory_order_acq_rel);
        if (s) {
            io_priority_class p;
            _registered_names[i] = name;
            p.val = i;
            return p;
        };
    }
    throw std::runtime_error("No more room for new I/O priority classes");
}

seastar::metrics::label io_queue_shard("ioshard");

io_queue::priority_class_data::priority_class_data(sstring name, sstring mountpoint, priority_class_ptr ptr, shard_id owner)
    : ptr(ptr)
    , bytes(0)
    , ops(0)
    , nr_queued(0)
    , queue_time(1s)
{
    namespace sm = seastar::metrics;
    auto shard = sm::impl::shard();

    auto ioq_group = sm::label("mountpoint");
    auto mountlabel = ioq_group(mountpoint);

    auto class_label_type = sm::label("class");
    auto class_label = class_label_type(name);
    _metric_groups.add_group("io_queue", {
            sm::make_derive("total_bytes", bytes, sm::description("Total bytes passed in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_derive("total_operations", ops, sm::description("Total bytes passed in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            // Note: The counter below is not the same as reactor's queued-io-requests
            // queued-io-requests shows us how many requests in total exist in this I/O Queue.
            //
            // This counter lives in the priority class, so it will count only queued requests
            // that belong to that class.
            //
            // In other words: the new counter tells you how busy a class is, and the
            // old counter tells you how busy the system is.

            sm::make_queue_length("queue_length", nr_queued, sm::description("Number of requests in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_gauge("delay", [this] {
                return queue_time.count();
            }, sm::description("total delay time in the queue"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label}),
            sm::make_gauge("shares", [this] {
                return this->ptr->shares();
            }, sm::description("current amount of shares"), {io_queue_shard(shard), sm::shard_label(owner), mountlabel, class_label})
    });
}

io_queue::priority_class_data& io_queue::find_or_create_class(const io_priority_class& pc, shard_id owner) {
    auto it_pclass = _priority_classes.find(pc.id());
    if (it_pclass == _priority_classes.end()) {
        auto shares = _registered_shares.at(pc.id()).load(std::memory_order_acquire);
        auto name = _registered_names.at(pc.id());
        // A note on naming:
        //
        // We could just add the owner as the instance id and have something like:
        //  io_queue-<class_owner>-<counter>-<class_name>
        //
        // However, when there are more than one shard per I/O queue, it is very useful
        // to know which shards are being served by the same queue. Therefore, a better name
        // scheme is:
        //
        //  io_queue-<queue_owner>-<counter>-<class_name>, shard=<class_owner>
        //  using the shard label to hold the owner number
        //
        // This conveys all the information we need and allows one to easily group all classes from
        // the same I/O queue (by filtering by shard)

        auto ret = _priority_classes.emplace(pc.id(), make_lw_shared<priority_class_data>(name, mountpoint(), _fq.register_priority_class(shares), owner));
        it_pclass = ret.first;
    }
    return *(it_pclass->second);
}

template <typename Func>
future<io_event>
io_queue::queue_request(const io_priority_class& pc, size_t len, io_queue::request_type req_type, Func prepare_io) {
    auto start = std::chrono::steady_clock::now();
    return smp::submit_to(coordinator(), [start, &pc, len, req_type, prepare_io = std::move(prepare_io), owner = engine().cpu_id(), this] {
        // First time will hit here, and then we create the class. It is important
        // that we create the shared pointer in the same shard it will be used at later.
        auto& pclass = find_or_create_class(pc, owner);
        pclass.nr_queued++;
        unsigned weight;
        size_t size;
        if (req_type == io_queue::request_type::write) {
            weight = _config.disk_req_write_to_read_multiplier;
            size = _config.disk_bytes_write_to_read_multiplier * len;
        } else {
            weight = io_queue::read_request_base_count;
            size = io_queue::read_request_base_count * len;
        }
        auto desc = std::make_unique<io_desc>(this, weight, size);
        auto fq_desc = desc->fq_descriptor();
        auto fut = desc->get_future();
        _fq.queue(pclass.ptr, std::move(fq_desc), [&pclass, start, prepare_io = std::move(prepare_io), desc = std::move(desc), len, this] () mutable noexcept {
            try {
                pclass.nr_queued--;
                pclass.ops++;
                pclass.bytes += len;
                pclass.queue_time = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start);
                engine().submit_io(desc.get(), std::move(prepare_io));
                desc.release();
            } catch (...) {
                desc->set_exception(std::current_exception());
                notify_requests_finished(desc->fq_descriptor());
            }
        });
        return fut;
    });
}

future<>
io_queue::update_shares_for_class(const io_priority_class pc, size_t new_shares) {
    return smp::submit_to(coordinator(), [this, pc, owner = engine().cpu_id(), new_shares] {
        auto& pclass = find_or_create_class(pc, owner);
        _fq.update_shares(pclass.ptr, new_shares);
    });
}

file_impl* file_impl::get_file_impl(file& f) {
    return f._file_impl.get();
}

posix_file_impl::posix_file_impl(int fd, file_open_options options, io_queue* ioq)
        : _io_queue(ioq)
        , _fd(fd)
{
    query_dma_alignment();
}

posix_file_impl::~posix_file_impl() {
    if (_refcount && _refcount->fetch_add(-1, std::memory_order_relaxed) != 1) {
        return;
    }
    delete _refcount;
    if (_fd != -1) {
        // Note: close() can be a blocking operation on NFS
        ::close(_fd);
    }
}

void
posix_file_impl::query_dma_alignment() {
    dioattr da;
    auto r = ioctl(_fd, XFS_IOC_DIOINFO, &da);
    if (r == 0) {
        _memory_dma_alignment = da.d_mem;
        _disk_read_dma_alignment = da.d_miniosz;
        // xfs wants at least the block size for writes
        // FIXME: really read the block size
        _disk_write_dma_alignment = std::max<unsigned>(da.d_miniosz, 4096);
    }
}

future<size_t>
posix_file_impl::write_dma(uint64_t pos, const void* buffer, size_t len, const io_priority_class& io_priority_class) {
    return engine().submit_io_write(_io_queue, io_priority_class, len, [fd = _fd, pos, buffer, len] (iocb& io) {
        io = make_write_iocb(fd, pos, const_cast<void*>(buffer), len);
    }).then([] (io_event ev) {
        engine().handle_io_result(ev);
        return make_ready_future<size_t>(size_t(ev.res));
    });
}

future<size_t>
posix_file_impl::write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& io_priority_class) {
    auto len = boost::accumulate(iov | boost::adaptors::transformed(std::mem_fn(&iovec::iov_len)), size_t(0));
    auto iov_ptr = std::make_unique<std::vector<iovec>>(std::move(iov));
    auto size = iov_ptr->size();
    auto data = iov_ptr->data();
    return engine().submit_io_write(_io_queue, io_priority_class, len, [fd = _fd, pos, data, size] (iocb& io) {
        io = make_writev_iocb(fd, pos, data, size);
    }).then([iov_ptr = std::move(iov_ptr)] (io_event ev) {
        engine().handle_io_result(ev);
        return make_ready_future<size_t>(size_t(ev.res));
    });
}

future<size_t>
posix_file_impl::read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& io_priority_class) {
    return engine().submit_io_read(_io_queue, io_priority_class, len, [fd = _fd, pos, buffer, len] (iocb& io) {
        io = make_read_iocb(fd, pos, buffer, len);
    }).then([] (io_event ev) {
        engine().handle_io_result(ev);
        return make_ready_future<size_t>(size_t(ev.res));
    });
}

future<size_t>
posix_file_impl::read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& io_priority_class) {
    auto len = boost::accumulate(iov | boost::adaptors::transformed(std::mem_fn(&iovec::iov_len)), size_t(0));
    auto iov_ptr = std::make_unique<std::vector<iovec>>(std::move(iov));
    auto size = iov_ptr->size();
    auto data = iov_ptr->data();
    return engine().submit_io_read(_io_queue, io_priority_class, len, [fd = _fd, pos, data, size] (iocb& io) {
        io = make_read_iocb(fd, pos, data, size);
    }).then([iov_ptr = std::move(iov_ptr)] (io_event ev) {
        engine().handle_io_result(ev);
        return make_ready_future<size_t>(size_t(ev.res));
    });
}

future<temporary_buffer<uint8_t>>
posix_file_impl::dma_read_bulk(uint64_t offset, size_t range_size, const io_priority_class& pc) {
    using tmp_buf_type = typename file::read_state<uint8_t>::tmp_buf_type;

    auto front = offset & (_disk_read_dma_alignment - 1);
    offset -= front;
    range_size += front;

    auto rstate = make_lw_shared<file::read_state<uint8_t>>(offset, front,
                                                       range_size,
                                                       _memory_dma_alignment,
                                                       _disk_read_dma_alignment);

    //
    // First, try to read directly into the buffer. Most of the reads will
    // end here.
    //
    auto read = read_dma(offset, rstate->buf.get_write(),
                         rstate->buf.size(), pc);

    return read.then([rstate, this, &pc] (size_t size) mutable {
        rstate->pos = size;

        //
        // If we haven't read all required data at once -
        // start read-copy sequence. We can't continue with direct reads
        // into the previously allocated buffer here since we have to ensure
        // the aligned read length and thus the aligned destination buffer
        // size.
        //
        // The copying will actually take place only if there was a HW glitch.
        // In EOF case or in case of a persistent I/O error the only overhead is
        // an extra allocation.
        //
        return do_until(
            [rstate] { return rstate->done(); },
            [rstate, this, &pc] () mutable {
            return read_maybe_eof(
                rstate->cur_offset(), rstate->left_to_read(), pc).then(
                    [rstate] (auto buf1) mutable {
                if (buf1.size()) {
                    rstate->append_new_data(buf1);
                } else {
                    rstate->eof = true;
                }

                return make_ready_future<>();
            });
        }).then([rstate] () mutable {
            //
            // If we are here we are promised to have read some bytes beyond
            // "front" so we may trim straight away.
            //
            rstate->trim_buf_before_ret();
            return make_ready_future<tmp_buf_type>(std::move(rstate->buf));
        });
    });
}

future<temporary_buffer<uint8_t>>
posix_file_impl::read_maybe_eof(uint64_t pos, size_t len, const io_priority_class& pc) {
    //
    // We have to allocate a new aligned buffer to make sure we don't get
    // an EINVAL error due to unaligned destination buffer.
    //
    temporary_buffer<uint8_t> buf = temporary_buffer<uint8_t>::aligned(
               _memory_dma_alignment, align_up(len, size_t(_disk_read_dma_alignment)));

    // try to read a single bulk from the given position
    auto dst = buf.get_write();
    auto buf_size = buf.size();
    return read_dma(pos, dst, buf_size, pc).then_wrapped(
            [buf = std::move(buf)](future<size_t> f) mutable {
        try {
            size_t size = std::get<0>(f.get());

            buf.trim(size);

            return std::move(buf);
        } catch (std::system_error& e) {
            //
            // TODO: implement a non-trowing file_impl::dma_read() interface to
            //       avoid the exceptions throwing in a good flow completely.
            //       Otherwise for users that don't want to care about the
            //       underlying file size and preventing the attempts to read
            //       bytes beyond EOF there will always be at least one
            //       exception throwing at the file end for files with unaligned
            //       length.
            //
            if (e.code().value() == EINVAL) {
                buf.trim(0);
                return std::move(buf);
            } else {
                throw;
            }
        }
    });
}

append_challenged_posix_file_impl::append_challenged_posix_file_impl(int fd, file_open_options options,
        unsigned max_size_changing_ops, bool fsync_is_exclusive, io_queue* ioq)
        : posix_file_impl(fd, options, ioq)
        , _max_size_changing_ops(max_size_changing_ops)
        , _fsync_is_exclusive(fsync_is_exclusive) {
    auto r = ::lseek(fd, 0, SEEK_END);
    throw_system_error_on(r == -1);
    _committed_size = _logical_size = r;
    _sloppy_size = options.sloppy_size;
    auto hint = align_up<uint64_t>(options.sloppy_size_hint, _disk_write_dma_alignment);
    if (_sloppy_size && _committed_size < hint) {
        auto r = ::ftruncate(_fd, hint);
        // We can ignore errors, since it's just a hint.
        if (r != -1) {
            _committed_size = hint;
        }
    }
}

append_challenged_posix_file_impl::~append_challenged_posix_file_impl() {
}

bool
append_challenged_posix_file_impl::must_run_alone(const op& candidate) const noexcept {
    // checks if candidate is a non-write, size-changing operation.
    return (candidate.type == opcode::truncate)
            || (candidate.type == opcode::flush && (_fsync_is_exclusive || _sloppy_size));
}

bool
append_challenged_posix_file_impl::size_changing(const op& candidate) const noexcept {
    return (candidate.type == opcode::write && candidate.pos + candidate.len > _committed_size)
            || must_run_alone(candidate);
}

bool
append_challenged_posix_file_impl::may_dispatch(const op& candidate) const noexcept {
    if (size_changing(candidate)) {
        return !_current_size_changing_ops && !_current_non_size_changing_ops;
    } else {
        return !_current_size_changing_ops;
    }
}

void
append_challenged_posix_file_impl::dispatch(op& candidate) noexcept {
    unsigned* op_counter = size_changing(candidate)
            ? &_current_size_changing_ops : &_current_non_size_changing_ops;
    ++*op_counter;
    candidate.run().then([me = shared_from_this(), op_counter] {
        --*op_counter;
        me->process_queue();
    });
}

// If we have a bunch of size-extending writes in the queue,
// issue an ftruncate() extending the file size, so they can
// be issued concurrently.
void
append_challenged_posix_file_impl::optimize_queue() noexcept {
    if (_current_non_size_changing_ops || _current_size_changing_ops) {
        // Can't issue an ftruncate() if something is going on
        return;
    }
    auto speculative_size = _committed_size;
    unsigned n_appending_writes = 0;
    for (const auto& op : _q) {
        // stop calculating speculative size after a non-write, size-changing
        // operation is found to prevent an useless truncate from being issued.
        if (must_run_alone(op)) {
            break;
        }
        if (op.type == opcode::write && op.pos + op.len > _committed_size) {
            speculative_size = std::max(speculative_size, op.pos + op.len);
            ++n_appending_writes;
        }
    }
    if (n_appending_writes > _max_size_changing_ops
            || (n_appending_writes && _sloppy_size)) {
        if (_sloppy_size && speculative_size < 2 * _committed_size) {
            speculative_size = align_up<uint64_t>(2 * _committed_size, _disk_write_dma_alignment);
        }
        // We're all alone, so issuing the ftruncate() in the reactor
        // thread won't block us.
        //
        // Issuing it in the syscall thread is too slow; this can happen
        // every several ops, and the syscall thread latency can be very
        // high.
        auto r = ::ftruncate(_fd, speculative_size);
        if (r != -1) {
            _committed_size = speculative_size;
            // If we failed, the next write will pick it up.
        }
    }
}

void
append_challenged_posix_file_impl::process_queue() noexcept {
    optimize_queue();
    while (!_q.empty() && may_dispatch(_q.front())) {
        op candidate = std::move(_q.front());
        _q.pop_front();
        dispatch(candidate);
    }
    if (may_quit()) {
        _completed.set_value();
        _done = false; // prevents _completed to be signaled again in case of recursion
    }
}

void
append_challenged_posix_file_impl::enqueue(op&& op) {
    _q.push_back(std::move(op));
    process_queue();
}

bool
append_challenged_posix_file_impl::may_quit() const noexcept {
    return _done && _q.empty() && !_current_non_size_changing_ops && !_current_size_changing_ops;
}

void
append_challenged_posix_file_impl::commit_size(uint64_t size) noexcept {
    _committed_size = std::max(size, _committed_size);
    _logical_size = std::max(size, _logical_size);
}

future<size_t>
append_challenged_posix_file_impl::read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& pc) {
    if (pos >= _logical_size) {
        // later() avoids tail recursion
        return later().then([] {
            return size_t(0);
        });
    }
    len = std::min(pos + len, align_up<uint64_t>(_logical_size, _disk_read_dma_alignment)) - pos;
    auto pr = make_lw_shared(promise<size_t>());
    enqueue({
        opcode::read,
        pos,
        len,
        [this, pr, pos, buffer, len, &pc] {
            return futurize_apply([this, pos, buffer, len, &pc] () mutable {
                return posix_file_impl::read_dma(pos, buffer, len, pc);
            }).then_wrapped([pr] (future<size_t> f) {
                f.forward_to(std::move(*pr));
            });
        }
    });
    return pr->get_future();
}

future<size_t>
append_challenged_posix_file_impl::read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) {
    if (pos >= _logical_size) {
        // later() avoids tail recursion
        return later().then([] {
            return size_t(0);
        });
    }
    size_t len = 0;
    auto i = iov.begin();
    while (i != iov.end() && pos + len + i->iov_len <= _logical_size) {
        len += i++->iov_len;
    }
    auto aligned_logical_size = align_up<uint64_t>(_logical_size, _disk_read_dma_alignment);
    if (i != iov.end()) {
        auto last_len = pos + len + i->iov_len - aligned_logical_size;
        if (last_len) {
            i++->iov_len = last_len;
        }
        iov.erase(i, iov.end());
    }
    auto pr = make_lw_shared(promise<size_t>());
    enqueue({
        opcode::read,
        pos,
        len,
        [this, pr, pos, iov = std::move(iov), &pc] () mutable {
            return futurize_apply([this, pos, iov = std::move(iov), &pc] () mutable {
                return posix_file_impl::read_dma(pos, std::move(iov), pc);
            }).then_wrapped([pr] (future<size_t> f) {
                f.forward_to(std::move(*pr));
            });
        }
    });
    return pr->get_future();
}

future<size_t>
append_challenged_posix_file_impl::write_dma(uint64_t pos, const void* buffer, size_t len, const io_priority_class& pc) {
    auto pr = make_lw_shared(promise<size_t>());
    enqueue({
        opcode::write,
        pos,
        len,
        [this, pr, pos, buffer, len, &pc] {
            return futurize_apply([this, pos, buffer, len, &pc] () mutable {
                return posix_file_impl::write_dma(pos, buffer, len, pc);
            }).then_wrapped([this, pos, pr] (future<size_t> f) {
                if (!f.failed()) {
                    auto ret = f.get0();
                    commit_size(pos + ret);
                    // Can't use forward_to(), because future::get0() invalidates the future.
                    pr->set_value(ret);
                } else {
                    f.forward_to(std::move(*pr));
                }
            });
        }
    });
    return pr->get_future();
}

future<size_t>
append_challenged_posix_file_impl::write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) {
    auto pr = make_lw_shared(promise<size_t>());
    auto len = boost::accumulate(iov | boost::adaptors::transformed(std::mem_fn(&iovec::iov_len)), size_t(0));
    enqueue({
        opcode::write,
        pos,
        len,
        [this, pr, pos, iov = std::move(iov), &pc] () mutable {
            return futurize_apply([this, pos, iov = std::move(iov), &pc] () mutable {
                return posix_file_impl::write_dma(pos, std::move(iov), pc);
            }).then_wrapped([this, pos, pr] (future<size_t> f) {
                if (!f.failed()) {
                    auto ret = f.get0();
                    commit_size(pos + ret);
                    // Can't use forward_to(), because future::get0() invalidates the future.
                    pr->set_value(ret);
                } else {
                    f.forward_to(std::move(*pr));
                }
            });
        }
    });
    return pr->get_future();
}

future<>
append_challenged_posix_file_impl::flush() {
    if ((!_sloppy_size || _logical_size == _committed_size) && !_fsync_is_exclusive) {
        // FIXME: determine if flush can block concurrent reads or writes
        return posix_file_impl::flush();
    } else {
        auto pr = make_lw_shared(promise<>());
        enqueue({
            opcode::flush,
            0,
            0,
            [this, pr] () {
                return futurize_apply([this] {
                    if (_logical_size != _committed_size) {
                        // We're all alone, so can truncate in reactor thread
                        auto r = ::ftruncate(_fd, _logical_size);
                        throw_system_error_on(r == -1);
                        _committed_size = _logical_size;
                    }
                    return posix_file_impl::flush();
                }).then_wrapped([pr] (future<> f) {
                    f.forward_to(std::move(*pr));
                });
            }
        });
        return pr->get_future();
    }
}

future<struct stat>
append_challenged_posix_file_impl::stat() {
    // FIXME: can this conflict with anything?
    return posix_file_impl::stat().then([this] (struct stat stat) {
        stat.st_size = _logical_size;
        return stat;
    });
}

future<>
append_challenged_posix_file_impl::truncate(uint64_t length) {
    auto pr = make_lw_shared(promise<>());
    enqueue({
        opcode::truncate,
        length,
        0,
        [this, pr, length] () mutable {
            return futurize_apply([this, length] {
                return posix_file_impl::truncate(length);
            }).then_wrapped([this, pr, length] (future<> f) {
                if (!f.failed()) {
                    _committed_size = _logical_size = length;
                }
                f.forward_to(std::move(*pr));
            });
        }
    });
    return pr->get_future();
}

future<uint64_t>
append_challenged_posix_file_impl::size() {
    return make_ready_future<size_t>(_logical_size);
}

future<>
append_challenged_posix_file_impl::close() noexcept {
    // Caller should have drained all pending I/O
    _done = true;
    process_queue();
    return _completed.get_future().then([this] {
        if (_logical_size != _committed_size) {
            auto r = ::ftruncate(_fd, _logical_size);
            if (r != -1) {
                _committed_size = _logical_size;
            }
        }
        return posix_file_impl::close();
    });
}

// Some kernels can append to xfs filesystems, some cannot; determine
// from kernel version.
static
unsigned
xfs_concurrency_from_kernel_version() {
    auto num = [] (std::csub_match x) {
        auto b = x.first;
        auto e = x.second;
        if (*b == '.') {
            ++b;
        }
        return std::stoi(std::string(b, e));
    };
    struct utsname buf;
    auto r = ::uname(&buf);
    throw_system_error_on(r == -1);
    // 2-4 dotted decimal numbers, optional "-anything"
    auto generic_re = std::regex(R"XX((\d+)(\.\d+)(\.\d+)?(\.\d+)?(-.*)?)XX");
    std::cmatch m1;
    // try to see if this is a mainline kernel with xfs append fixed (3.15+)
    if (std::regex_match(buf.release, m1, generic_re)) {
        auto maj = num(m1[1]);
        auto min = num(m1[2]);
        if (maj > 3 || (maj == 3 && min >= 15)) {
            // Can append, but not concurrently
            return 1;
        }
    }
    // 3.10.0-num1.num2?.num3?.el7.anything
    auto rhel_re = std::regex(R"XX(3\.10\.0-(\d+)(\.\d+)?(\.\d+)?\.el7.*)XX");
    std::cmatch m2;
    // try to see if this is a RHEL kernel with the backported fix (3.10.0-325.el7+)
    if (std::regex_match(buf.release, m2, rhel_re)) {
        auto rmaj = num(m2[1]);
        if (rmaj >= 325) {
            // Can append, but not concurrently
            return 1;
        }
    }
    // Cannot append at all; need ftrucnate().
    return 0;
}

inline
shared_ptr<file_impl>
make_file_impl(int fd, file_open_options options) {
    struct stat st;
    auto r = ::fstat(fd, &st);
    throw_system_error_on(r == -1);

    r = ::ioctl(fd, BLKGETSIZE);
    io_queue& io_queue = engine().get_io_queue(st.st_dev);
    if (r != -1) {
        return make_shared<blockdev_file_impl>(fd, options, &io_queue);
    } else {
        // FIXME: obtain these flags from somewhere else
        auto flags = ::fcntl(fd, F_GETFL);
        throw_system_error_on(flags == -1);
        if ((flags & O_ACCMODE) == O_RDONLY) {
            return make_shared<posix_file_impl>(fd, options, &io_queue);
        }
        if (S_ISDIR(st.st_mode)) {
            return make_shared<posix_file_impl>(fd, options, &io_queue);
        }
        struct append_support {
            bool append_challenged;
            unsigned append_concurrency;
            bool fsync_is_exclusive;
        };
        static thread_local std::unordered_map<decltype(st.st_dev), append_support> s_fstype;
        if (!s_fstype.count(st.st_dev)) {
            struct statfs sfs;
            auto r = ::fstatfs(fd, &sfs);
            throw_system_error_on(r == -1);
            append_support as;
            switch (sfs.f_type) {
            case 0x58465342: /* XFS */
                as.append_challenged = true;
                static auto xc = xfs_concurrency_from_kernel_version();
                as.append_concurrency = xc;
                as.fsync_is_exclusive = true;
                break;
            case 0x6969: /* NFS */
                as.append_challenged = false;
                as.append_concurrency = 0;
                as.fsync_is_exclusive = false;
                break;
            case 0xEF53: /* EXT4 */
                as.append_challenged = true;
                as.append_concurrency = 0;
                as.fsync_is_exclusive = false;
                break;
            default:
                as.append_challenged = true;
                as.append_concurrency = 0;
                as.fsync_is_exclusive = true;
            }
            s_fstype[st.st_dev] = as;
        }
        auto as = s_fstype[st.st_dev];
        if (!as.append_challenged) {
            return make_shared<posix_file_impl>(fd, options, &io_queue);
        }
        return make_shared<append_challenged_posix_file_impl>(fd, options, as.append_concurrency, as.fsync_is_exclusive, &io_queue);
    }
}

file::file(int fd, file_open_options options)
        : _file_impl(make_file_impl(fd, options)) {
}

future<file>
reactor::open_file_dma(sstring name, open_flags flags, file_open_options options) {
    static constexpr mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH; // 0644
    return _thread_pool.submit<syscall_result<int>>([name, flags, options, strict_o_direct = _strict_o_direct] {
        // We want O_DIRECT, except in two cases:
        //   - tmpfs (which doesn't support it, but works fine anyway)
        //   - strict_o_direct == false (where we forgive it being not supported)
        // Because open() with O_DIRECT will fail, we open it without O_DIRECT, try
        // to update it to O_DIRECT with fcntl(), and if that fails, see if we
        // can forgive it.
        auto is_tmpfs = [] (int fd) {
            struct ::statfs buf;
            auto r = ::fstatfs(fd, &buf);
            if (r == -1) {
                return false;
            }
            return buf.f_type == 0x01021994; // TMPFS_MAGIC
        };
        auto open_flags = O_CLOEXEC | static_cast<int>(flags);
        int fd = ::open(name.c_str(), open_flags, mode);
        if (fd == -1) {
            return wrap_syscall<int>(fd);
        }
        int r = ::fcntl(fd, F_SETFL, open_flags | O_DIRECT);
        auto maybe_ret = wrap_syscall<int>(r);  // capture errno (should be EINVAL)
        if (r == -1  && strict_o_direct && !is_tmpfs(fd)) {
            ::close(fd);
            return maybe_ret;
        }
        if (fd != -1) {
            fsxattr attr = {};
            if (options.extent_allocation_size_hint) {
                attr.fsx_xflags |= XFS_XFLAG_EXTSIZE;
                attr.fsx_extsize = options.extent_allocation_size_hint;
            }
            // Ignore error; may be !xfs, and just a hint anyway
            ::ioctl(fd, XFS_IOC_FSSETXATTR, &attr);
        }
        return wrap_syscall<int>(fd);
    }).then([options, name] (syscall_result<int> sr) {
        sr.throw_fs_exception_if_error("open failed", name);
        return make_ready_future<file>(file(sr.result, options));
    });
}

future<>
reactor::remove_file(sstring pathname) {
    return engine()._thread_pool.submit<syscall_result<int>>([pathname] {
        return wrap_syscall<int>(::remove(pathname.c_str()));
    }).then([pathname] (syscall_result<int> sr) {
        sr.throw_fs_exception_if_error("remove failed", pathname);
        return make_ready_future<>();
    });
}

future<>
reactor::rename_file(sstring old_pathname, sstring new_pathname) {
    return engine()._thread_pool.submit<syscall_result<int>>([old_pathname, new_pathname] {
        return wrap_syscall<int>(::rename(old_pathname.c_str(), new_pathname.c_str()));
    }).then([old_pathname, new_pathname] (syscall_result<int> sr) {
        sr.throw_fs_exception_if_error("rename failed",  old_pathname, new_pathname);
        return make_ready_future<>();
    });
}

future<>
reactor::link_file(sstring oldpath, sstring newpath) {
    return engine()._thread_pool.submit<syscall_result<int>>([oldpath, newpath] {
        return wrap_syscall<int>(::link(oldpath.c_str(), newpath.c_str()));
    }).then([oldpath, newpath] (syscall_result<int> sr) {
        sr.throw_fs_exception_if_error("link failed", oldpath, newpath);
        return make_ready_future<>();
    });
}

directory_entry_type stat_to_entry_type(__mode_t type) {
    if (S_ISDIR(type)) {
        return directory_entry_type::directory;
    }
    if (S_ISBLK(type)) {
        return directory_entry_type::block_device;
    }
    if (S_ISCHR(type)) {
            return directory_entry_type::char_device;
    }
    if (S_ISFIFO(type)) {
        return directory_entry_type::fifo;
    }
    if (S_ISLNK(type)) {
        return directory_entry_type::link;
    }
    return directory_entry_type::regular;

}

future<compat::optional<directory_entry_type>>
reactor::file_type(sstring name) {
    return _thread_pool.submit<syscall_result_extra<struct stat>>([name] {
        struct stat st;
        auto ret = stat(name.c_str(), &st);
        return wrap_syscall(ret, st);
    }).then([name] (syscall_result_extra<struct stat> sr) {
        if (long(sr.result) == -1) {
            if (sr.error != ENOENT && sr.error != ENOTDIR) {
                sr.throw_fs_exception_if_error("stat failed", name);
            }
            return make_ready_future<compat::optional<directory_entry_type> >
                (compat::optional<directory_entry_type>() );
        }
        return make_ready_future<compat::optional<directory_entry_type> >
            (compat::optional<directory_entry_type>(stat_to_entry_type(sr.extra.st_mode)) );
    });
}

future<uint64_t>
reactor::file_size(sstring pathname) {
    return _thread_pool.submit<syscall_result_extra<struct stat>>([pathname] {
        struct stat st;
        auto ret = stat(pathname.c_str(), &st);
        return wrap_syscall(ret, st);
    }).then([pathname] (syscall_result_extra<struct stat> sr) {
        sr.throw_fs_exception_if_error("stat failed", pathname);
        return make_ready_future<uint64_t>(sr.extra.st_size);
    });
}

future<bool>
reactor::file_accessible(sstring pathname, access_flags flags) {
    return _thread_pool.submit<syscall_result<int>>([pathname, flags] {
        auto aflags = std::underlying_type_t<access_flags>(flags);
        auto ret = ::access(pathname.c_str(), aflags);
        return wrap_syscall(ret);
    }).then([this, pathname, flags] (syscall_result<int> sr) {
        if (sr.result < 0) {
            if ((sr.error == ENOENT && flags == access_flags::exists) ||
                (sr.error == EACCES && flags != access_flags::exists)) {
                return make_ready_future<bool>(false);
            }
            sr.throw_fs_exception("access failed", fs::path(pathname));
        }

        return make_ready_future<bool>(true);
    });
}

future<fs_type>
reactor::file_system_at(sstring pathname) {
    return _thread_pool.submit<syscall_result_extra<struct statfs>>([pathname] {
        struct statfs st;
        auto ret = statfs(pathname.c_str(), &st);
        return wrap_syscall(ret, st);
    }).then([pathname] (syscall_result_extra<struct statfs> sr) {
        static std::unordered_map<long int, fs_type> type_mapper = {
            { 0x58465342, fs_type::xfs },
            { EXT2_SUPER_MAGIC, fs_type::ext2 },
            { EXT3_SUPER_MAGIC, fs_type::ext3 },
            { EXT4_SUPER_MAGIC, fs_type::ext4 },
            { BTRFS_SUPER_MAGIC, fs_type::btrfs },
            { 0x4244, fs_type::hfs },
            { TMPFS_MAGIC, fs_type::tmpfs },
        };
        sr.throw_fs_exception_if_error("statfs failed", pathname);

        fs_type ret = fs_type::other;
        if (type_mapper.count(sr.extra.f_type) != 0) {
            ret = type_mapper.at(sr.extra.f_type);
        }
        return make_ready_future<fs_type>(ret);
    });
}

future<struct statvfs>
reactor::statvfs(sstring pathname) {
    return _thread_pool.submit<syscall_result_extra<struct statvfs>>([pathname] {
        struct statvfs st;
        auto ret = ::statvfs(pathname.c_str(), &st);
        return wrap_syscall(ret, st);
    }).then([pathname] (syscall_result_extra<struct statvfs> sr) {
        sr.throw_fs_exception_if_error("statvfs failed", pathname);
        struct statvfs st = sr.extra;
        return make_ready_future<struct statvfs>(std::move(st));
    });
}

future<file>
reactor::open_directory(sstring name) {
    return _thread_pool.submit<syscall_result<int>>([name] {
        return wrap_syscall<int>(::open(name.c_str(), O_DIRECTORY | O_CLOEXEC | O_RDONLY));
    }).then([name] (syscall_result<int> sr) {
        sr.throw_fs_exception_if_error("open failed", name);
        return make_ready_future<file>(file(sr.result, file_open_options()));
    });
}

future<>
reactor::make_directory(sstring name) {
    return _thread_pool.submit<syscall_result<int>>([name] {
        return wrap_syscall<int>(::mkdir(name.c_str(), S_IRWXU));
    }).then([name] (syscall_result<int> sr) {
        sr.throw_fs_exception_if_error("mkdir failed", name);
    });
}

future<>
reactor::touch_directory(sstring name) {
    return engine()._thread_pool.submit<syscall_result<int>>([name] {
        return wrap_syscall<int>(::mkdir(name.c_str(), S_IRWXU));
    }).then([name] (syscall_result<int> sr) {
        if (sr.error != EEXIST) {
            sr.throw_fs_exception_if_error("mkdir failed", name);
        }
    });
}

file_handle::file_handle(const file_handle& x)
        : _impl(x._impl ? x._impl->clone() : std::unique_ptr<file_handle_impl>()) {
}

file_handle::file_handle(file_handle&& x) noexcept = default;

file_handle&
file_handle::operator=(const file_handle& x) {
    return operator=(file_handle(x));
}

file_handle&
file_handle::operator=(file_handle&&) noexcept = default;

file
file_handle::to_file() const & {
    return file_handle(*this).to_file();
}

file
file_handle::to_file() && {
    return file(std::move(*_impl).to_file());
}

file::file(seastar::file_handle&& handle)
        : _file_impl(std::move(std::move(handle).to_file()._file_impl)) {
}

seastar::file_handle
file::dup() {
    return seastar::file_handle(_file_impl->dup());
}

std::unique_ptr<seastar::file_handle_impl>
file_impl::dup() {
    throw std::runtime_error("this file type cannot be duplicated");
}

std::unique_ptr<seastar::file_handle_impl>
posix_file_impl::dup() {
    if (!_refcount) {
        _refcount = new std::atomic<unsigned>(1u);
    }
    auto ret = std::make_unique<posix_file_handle_impl>(_fd, _refcount, _io_queue);
    _refcount->fetch_add(1, std::memory_order_relaxed);
    return std::move(ret);
}

posix_file_impl::posix_file_impl(int fd, std::atomic<unsigned>* refcount, io_queue *ioq)
        : _refcount(refcount), _io_queue(ioq), _fd(fd) {
}

posix_file_handle_impl::~posix_file_handle_impl() {
    if (_refcount && _refcount->fetch_add(-1, std::memory_order_relaxed) == 1) {
        ::close(_fd);
        delete _refcount;
    }
}

std::unique_ptr<seastar::file_handle_impl>
posix_file_handle_impl::clone() const {
    auto ret = std::make_unique<posix_file_handle_impl>(_fd, _refcount, _io_queue);
    if (_refcount) {
        _refcount->fetch_add(1, std::memory_order_relaxed);
    }
    return std::move(ret);
}

shared_ptr<file_impl>
posix_file_handle_impl::to_file() && {
    auto ret = ::seastar::make_shared<posix_file_impl>(_fd, _refcount, _io_queue);
    _fd = -1;
    _refcount = nullptr;
    return ret;
}

future<>
posix_file_impl::flush(void) {
    ++engine()._fsyncs;
    if (engine()._bypass_fsync) {
        return make_ready_future<>();
    }
    return engine()._thread_pool.submit<syscall_result<int>>([this] {
        return wrap_syscall<int>(::fdatasync(_fd));
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
        return make_ready_future<>();
    });
}

future<struct stat>
posix_file_impl::stat(void) {
    return engine()._thread_pool.submit<syscall_result_extra<struct stat>>([this] {
        struct stat st;
        auto ret = ::fstat(_fd, &st);
        return wrap_syscall(ret, st);
    }).then([] (syscall_result_extra<struct stat> ret) {
        ret.throw_if_error();
        return make_ready_future<struct stat>(ret.extra);
    });
}

future<>
posix_file_impl::truncate(uint64_t length) {
    return engine()._thread_pool.submit<syscall_result<int>>([this, length] {
        return wrap_syscall<int>(::ftruncate(_fd, length));
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
        return make_ready_future<>();
    });
}

blockdev_file_impl::blockdev_file_impl(int fd, file_open_options options, io_queue *ioq)
        : posix_file_impl(fd, options, ioq) {
}

future<>
blockdev_file_impl::truncate(uint64_t length) {
    return make_ready_future<>();
}

future<>
posix_file_impl::discard(uint64_t offset, uint64_t length) {
    return engine()._thread_pool.submit<syscall_result<int>>([this, offset, length] () mutable {
        return wrap_syscall<int>(::fallocate(_fd, FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,
            offset, length));
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
        return make_ready_future<>();
    });
}

future<>
posix_file_impl::allocate(uint64_t position, uint64_t length) {
#ifdef FALLOC_FL_ZERO_RANGE
    // FALLOC_FL_ZERO_RANGE is fairly new, so don't fail if it's not supported.
    static bool supported = true;
    if (!supported) {
        return make_ready_future<>();
    }
    return engine()._thread_pool.submit<syscall_result<int>>([this, position, length] () mutable {
        auto ret = ::fallocate(_fd, FALLOC_FL_ZERO_RANGE|FALLOC_FL_KEEP_SIZE, position, length);
        if (ret == -1 && errno == EOPNOTSUPP) {
            ret = 0;
            supported = false; // Racy, but harmless.  At most we issue an extra call or two.
        }
        return wrap_syscall<int>(ret);
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
        return make_ready_future<>();
    });
#else
    return make_ready_future<>();
#endif
}

future<>
blockdev_file_impl::discard(uint64_t offset, uint64_t length) {
    return engine()._thread_pool.submit<syscall_result<int>>([this, offset, length] () mutable {
        uint64_t range[2] { offset, length };
        return wrap_syscall<int>(::ioctl(_fd, BLKDISCARD, &range));
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
        return make_ready_future<>();
    });
}

future<>
blockdev_file_impl::allocate(uint64_t position, uint64_t length) {
    // nothing to do for block device
    return make_ready_future<>();
}

future<uint64_t>
posix_file_impl::size() {
    auto r = ::lseek(_fd, 0, SEEK_END);
    if (r == -1) {
        return make_exception_future<uint64_t>(std::system_error(errno, std::system_category()));
    }
    return make_ready_future<uint64_t>(r);
}

future<>
posix_file_impl::close() noexcept {
    if (_fd == -1) {
        seastar_logger.warn("double close() detected, contact support");
        return make_ready_future<>();
    }
    auto fd = _fd;
    _fd = -1;  // Prevent a concurrent close (which is illegal) from closing another file's fd
    if (_refcount && _refcount->fetch_add(-1, std::memory_order_relaxed) != 1) {
        _refcount = nullptr;
        return make_ready_future<>();
    }
    delete _refcount;
    _refcount = nullptr;
    auto closed = [fd] () noexcept {
        try {
            return engine()._thread_pool.submit<syscall_result<int>>([fd] {
                return wrap_syscall<int>(::close(fd));
            });
        } catch (...) {
            report_exception("Running ::close() in reactor thread, submission failed with exception", std::current_exception());
            return make_ready_future<syscall_result<int>>(wrap_syscall<int>(::close(fd)));
        }
    }();
    return closed.then([] (syscall_result<int> sr) {
        sr.throw_if_error();
    });
}

future<uint64_t>
blockdev_file_impl::size(void) {
    return engine()._thread_pool.submit<syscall_result_extra<size_t>>([this] {
        uint64_t size;
        int ret = ::ioctl(_fd, BLKGETSIZE64, &size);
        return wrap_syscall(ret, size);
    }).then([] (syscall_result_extra<uint64_t> ret) {
        ret.throw_if_error();
        return make_ready_future<uint64_t>(ret.extra);
    });
}

subscription<directory_entry>
posix_file_impl::list_directory(std::function<future<> (directory_entry de)> next) {
    static constexpr size_t buffer_size = 8192;
    struct work {
        stream<directory_entry> s;
        unsigned current = 0;
        unsigned total = 0;
        bool eof = false;
        int error = 0;
        char buffer[buffer_size];
    };

    // While it would be natural to use fdopendir()/readdir(),
    // our syscall thread pool doesn't support malloc(), which is
    // required for this to work.  So resort to using getdents()
    // instead.

    // From getdents(2):
    struct linux_dirent64 {
        ino64_t        d_ino;    /* 64-bit inode number */
        off64_t        d_off;    /* 64-bit offset to next structure */
        unsigned short d_reclen; /* Size of this dirent */
        unsigned char  d_type;   /* File type */
        char           d_name[]; /* Filename (null-terminated) */
    };

    auto w = make_lw_shared<work>();
    auto ret = w->s.listen(std::move(next));
    w->s.started().then([w, this] {
        auto eofcond = [w] { return w->eof; };
        return do_until(eofcond, [w, this] {
            if (w->current == w->total) {
                return engine()._thread_pool.submit<syscall_result<long>>([w , this] () {
                    auto ret = ::syscall(__NR_getdents64, _fd, reinterpret_cast<linux_dirent64*>(w->buffer), buffer_size);
                    return wrap_syscall(ret);
                }).then([w] (syscall_result<long> ret) {
                    ret.throw_if_error();
                    if (ret.result == 0) {
                        w->eof = true;
                    } else {
                        w->current = 0;
                        w->total = ret.result;
                    }
                });
            }
            auto start = w->buffer + w->current;
            auto de = reinterpret_cast<linux_dirent64*>(start);
            compat::optional<directory_entry_type> type;
            switch (de->d_type) {
            case DT_BLK:
                type = directory_entry_type::block_device;
                break;
            case DT_CHR:
                type = directory_entry_type::char_device;
                break;
            case DT_DIR:
                type = directory_entry_type::directory;
                break;
            case DT_FIFO:
                type = directory_entry_type::fifo;
                break;
            case DT_REG:
                type = directory_entry_type::regular;
                break;
            case DT_SOCK:
                type = directory_entry_type::socket;
                break;
            default:
                // unknown, ignore
                ;
            }
            w->current += de->d_reclen;
            sstring name = de->d_name;
            if (name == "." || name == "..") {
                return make_ready_future<>();
            }
            return w->s.produce({std::move(name), type});
        });
    }).then([w] {
        w->s.close();
    });
    return ret;
}

void reactor::enable_timer(steady_clock_type::time_point when)
{
#ifndef HAVE_OSV
    itimerspec its;
    its.it_interval = {};
    its.it_value = to_timespec(when);
    _backend->arm_highres_timer(its);
#else
    using ns = std::chrono::nanoseconds;
    WITH_LOCK(_timer_mutex) {
        _timer_due = std::chrono::duration_cast<ns>(when.time_since_epoch()).count();
        _timer_cond.wake_one();
    }
#endif
}

void reactor::add_timer(timer<steady_clock_type>* tmr) {
    if (queue_timer(tmr)) {
        enable_timer(_timers.get_next_timeout());
    }
}

bool reactor::queue_timer(timer<steady_clock_type>* tmr) {
    return _timers.insert(*tmr);
}

void reactor::del_timer(timer<steady_clock_type>* tmr) {
    if (tmr->_expired) {
        _expired_timers.erase(_expired_timers.iterator_to(*tmr));
        tmr->_expired = false;
    } else {
        _timers.remove(*tmr);
    }
}

void reactor::add_timer(timer<lowres_clock>* tmr) {
    if (queue_timer(tmr)) {
        _lowres_next_timeout = _lowres_timers.get_next_timeout();
    }
}

bool reactor::queue_timer(timer<lowres_clock>* tmr) {
    return _lowres_timers.insert(*tmr);
}

void reactor::del_timer(timer<lowres_clock>* tmr) {
    if (tmr->_expired) {
        _expired_lowres_timers.erase(_expired_lowres_timers.iterator_to(*tmr));
        tmr->_expired = false;
    } else {
        _lowres_timers.remove(*tmr);
    }
}

void reactor::add_timer(timer<manual_clock>* tmr) {
    queue_timer(tmr);
}

bool reactor::queue_timer(timer<manual_clock>* tmr) {
    return _manual_timers.insert(*tmr);
}

void reactor::del_timer(timer<manual_clock>* tmr) {
    if (tmr->_expired) {
        _expired_manual_timers.erase(_expired_manual_timers.iterator_to(*tmr));
        tmr->_expired = false;
    } else {
        _manual_timers.remove(*tmr);
    }
}

void reactor::at_exit(std::function<future<> ()> func) {
    assert(!_stopping);
    _exit_funcs.push_back(std::move(func));
}

future<> reactor::run_exit_tasks() {
    _stop_requested.broadcast();
    _stopping = true;
    stop_aio_eventfd_loop();
    return do_for_each(_exit_funcs.rbegin(), _exit_funcs.rend(), [] (auto& func) {
        return func();
    });
}

void reactor::stop() {
    assert(engine()._id == 0);
    smp::cleanup_cpu();
    if (!_stopping) {
        run_exit_tasks().then([this] {
            do_with(semaphore(0), [this] (semaphore& sem) {
                for (unsigned i = 1; i < smp::count; i++) {
                    smp::submit_to<>(i, []() {
                        smp::cleanup_cpu();
                        return engine().run_exit_tasks().then([] {
                                engine()._stopped = true;
                        });
                    }).then([&sem]() {
                        sem.signal();
                    });
                }
                return sem.wait(smp::count - 1).then([this] {
                    _stopped = true;
                });
            });
        });
    }
}

void reactor::exit(int ret) {
    smp::submit_to(0, [this, ret] { _return = ret; stop(); });
}

uint64_t
reactor::pending_task_count() const {
    uint64_t ret = 0;
    for (auto&& tq : _task_queues) {
        ret += tq->_q.size();
    }
    return ret;
}

uint64_t
reactor::tasks_processed() const {
    uint64_t ret = 0;
    for (auto&& tq : _task_queues) {
        ret += tq->_tasks_processed;
    }
    return ret;
}

void reactor::register_metrics() {

    namespace sm = seastar::metrics;

    _metric_groups.add_group("reactor", {
            sm::make_gauge("tasks_pending", std::bind(&reactor::pending_task_count, this), sm::description("Number of pending tasks in the queue")),
            // total_operations value:DERIVE:0:U
            sm::make_derive("tasks_processed", std::bind(&reactor::tasks_processed, this), sm::description("Total tasks processed")),
            sm::make_derive("polls", [this] { return _polls.load(std::memory_order_relaxed); }, sm::description("Number of times pollers were executed")),
            sm::make_derive("timers_pending", std::bind(&decltype(_timers)::size, &_timers), sm::description("Number of tasks in the timer-pending queue")),
            sm::make_gauge("utilization", [this] { return (1-_load)  * 100; }, sm::description("CPU utilization")),
            sm::make_derive("cpu_busy_ms", [this] () -> int64_t { return total_busy_time() / 1ms; },
                    sm::description("Total cpu busy time in milliseconds")),
            sm::make_derive("cpu_steal_time_ms", [this] () -> int64_t { return total_steal_time() / 1ms; },
                    sm::description("Total steal time, the time in which some other process was running while Seastar was not trying to run (not sleeping)."
                                     "Because this is in userspace, some time that could be legitimally thought as steal time is not accounted as such. For example, if we are sleeping and can wake up but the kernel hasn't woken us up yet.")),
            // total_operations value:DERIVE:0:U
            sm::make_derive("aio_reads", _io_stats.aio_reads, sm::description("Total aio-reads operations")),

            sm::make_total_bytes("aio_bytes_read", _io_stats.aio_read_bytes, sm::description("Total aio-reads bytes")),
            // total_operations value:DERIVE:0:U
            sm::make_derive("aio_writes", _io_stats.aio_writes, sm::description("Total aio-writes operations")),
            sm::make_total_bytes("aio_bytes_write", _io_stats.aio_write_bytes, sm::description("Total aio-writes bytes")),
            sm::make_derive("aio_errors", _io_stats.aio_errors, sm::description("Total aio errors")),
            // total_operations value:DERIVE:0:U
            sm::make_derive("fsyncs", _fsyncs, sm::description("Total number of fsync operations")),
            // total_operations value:DERIVE:0:U
            sm::make_derive("io_threaded_fallbacks", std::bind(&thread_pool::operation_count, &_thread_pool),
                    sm::description("Total number of io-threaded-fallbacks operations")),

    });

    _metric_groups.add_group("memory", {
            sm::make_derive("malloc_operations", [] { return memory::stats().mallocs(); },
                    sm::description("Total number of malloc operations")),
            sm::make_derive("free_operations", [] { return memory::stats().frees(); }, sm::description("Total number of free operations")),
            sm::make_derive("cross_cpu_free_operations", [] { return memory::stats().cross_cpu_frees(); }, sm::description("Total number of cross cpu free")),
            sm::make_gauge("malloc_live_objects", [] { return memory::stats().live_objects(); }, sm::description("Number of live objects")),
            sm::make_current_bytes("free_memory", [] { return memory::stats().free_memory(); }, sm::description("Free memeory size in bytes")),
            sm::make_current_bytes("total_memory", [] { return memory::stats().total_memory(); }, sm::description("Total memeory size in bytes")),
            sm::make_current_bytes("allocated_memory", [] { return memory::stats().allocated_memory(); }, sm::description("Allocated memeory size in bytes")),
            sm::make_derive("reclaims_operations", [] { return memory::stats().reclaims(); }, sm::description("Total reclaims operations"))
    });

    _metric_groups.add_group("reactor", {
            sm::make_derive("logging_failures", [] { return logging_failures; }, sm::description("Total number of logging failures")),
            // total_operations value:DERIVE:0:U
            sm::make_derive("cpp_exceptions", _cxx_exceptions, sm::description("Total number of C++ exceptions")),
    });

    auto ioq_group = sm::label("mountpoint");
    for (auto& ioq : my_io_queues) {
        auto ioq_name = ioq_group(ioq->mountpoint());
        _metric_groups.add_group("reactor", {
                sm::make_gauge("io_queue_requests", [&ioq] { return ioq->queued_requests(); } , sm::description("Number of requests in the io queue"), {ioq_name}),
        });
    }

    using namespace seastar::metrics;
    _metric_groups.add_group("reactor", {
        make_counter("fstream_reads", _io_stats.fstream_reads,
                description(
                        "Counts reads from disk file streams.  A high rate indicates high disk activity."
                        " Contrast with other fstream_read* counters to locate bottlenecks.")),
        make_derive("fstream_read_bytes", _io_stats.fstream_read_bytes,
                description(
                        "Counts bytes read from disk file streams.  A high rate indicates high disk activity."
                        " Divide by fstream_reads to determine average read size.")),
        make_counter("fstream_reads_blocked", _io_stats.fstream_reads_blocked,
                description(
                        "Counts the number of times a disk read could not be satisfied from read-ahead buffers, and had to block."
                        " Indicates short streams, or incorrect read ahead configuration.")),
        make_derive("fstream_read_bytes_blocked", _io_stats.fstream_read_bytes_blocked,
                description(
                        "Counts the number of bytes read from disk that could not be satisfied from read-ahead buffers, and had to block."
                        " Indicates short streams, or incorrect read ahead configuration.")),
        make_counter("fstream_reads_aheads_discarded", _io_stats.fstream_read_aheads_discarded,
                description(
                        "Counts the number of times a buffer that was read ahead of time and was discarded because it was not needed, wasting disk bandwidth."
                        " Indicates over-eager read ahead configuration.")),
        make_derive("fstream_reads_ahead_bytes_discarded", _io_stats.fstream_read_ahead_discarded_bytes,
                description(
                        "Counts the number of buffered bytes that were read ahead of time and were discarded because they were not needed, wasting disk bandwidth."
                        " Indicates over-eager read ahead configuration.")),
    });
}

void reactor::run_tasks(task_queue& tq) {
    // Make sure new tasks will inherit our scheduling group
    *internal::current_scheduling_group_ptr() = scheduling_group(tq._id);
    auto& tasks = tq._q;
    while (!tasks.empty()) {
        auto tsk = std::move(tasks.front());
        tasks.pop_front();
        STAP_PROBE(seastar, reactor_run_tasks_single_start);
        task_histogram_add_task(*tsk);
        tsk->run_and_dispose();
        tsk.release();
        STAP_PROBE(seastar, reactor_run_tasks_single_end);
        ++tq._tasks_processed;
        // check at end of loop, to allow at least one task to run
        if (need_preempt()) {
            if (tasks.size() <= _max_task_backlog) {
                break;
            } else {
                // While need_preempt() is set, task execution is inefficient due to
                // need_preempt() checks breaking out of loops and .then() calls. See
                // #302.
                reset_preemption_monitor();
            }
        }
    }
}

#ifdef SEASTAR_SHUFFLE_TASK_QUEUE
void reactor::shuffle(std::unique_ptr<task>& t, task_queue& q) {
    static thread_local std::mt19937 gen = std::mt19937(std::default_random_engine()());
    std::uniform_int_distribution<size_t> tasks_dist{0, q._q.size() - 1};
    auto& to_swap = q._q[tasks_dist(gen)];
    std::swap(to_swap, t);
}
#endif

void reactor::force_poll() {
    request_preemption();
}

bool
reactor::flush_tcp_batches() {
    bool work = _flush_batching.size();
    while (!_flush_batching.empty()) {
        auto os = std::move(_flush_batching.front());
        _flush_batching.pop_front();
        os->poll_flush();
    }
    return work;
}

bool
reactor::do_expire_lowres_timers() {
    if (_lowres_next_timeout == lowres_clock::time_point()) {
        return false;
    }
    auto now = lowres_clock::now();
    if (now >= _lowres_next_timeout) {
        complete_timers(_lowres_timers, _expired_lowres_timers, [this] {
            if (!_lowres_timers.empty()) {
                _lowres_next_timeout = _lowres_timers.get_next_timeout();
            } else {
                _lowres_next_timeout = lowres_clock::time_point();
            }
        });
        return true;
    }
    return false;
}

void
reactor::expire_manual_timers() {
    complete_timers(_manual_timers, _expired_manual_timers, [] {});
}

void
manual_clock::expire_timers() {
    local_engine->expire_manual_timers();
}

void
manual_clock::advance(manual_clock::duration d) {
    _now.fetch_add(d.count());
    if (local_engine) {
        schedule_urgent(make_task(default_scheduling_group(), &manual_clock::expire_timers));
        smp::invoke_on_all(&manual_clock::expire_timers);
    }
}

bool
reactor::do_check_lowres_timers() const {
    if (_lowres_next_timeout == lowres_clock::time_point()) {
        return false;
    }
    return lowres_clock::now() > _lowres_next_timeout;
}

#ifndef HAVE_OSV

class reactor::io_pollfn final : public reactor::pollfn {
    reactor& _r;
public:
    io_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() override final {
        return _r.process_io();
    }
    virtual bool pure_poll() override final {
        return poll(); // actually performs work, but triggers no user continuations, so okay
    }
    virtual bool try_enter_interrupt_mode() override {
        // Because aio depends on polling, it cannot generate events to wake us up, Therefore, sleep
        // is only possible if there are no in-flight aios. If there are, we need to keep polling.
        //
        // Alternatively, if we enabled _aio_eventfd, we can always enter
        unsigned executing = 0;
        for (auto& ioq : _r.my_io_queues) {
            executing += ioq->requests_currently_executing();
        }
        return executing == 0 || _r._aio_eventfd;
    }
    virtual void exit_interrupt_mode() override {
        // nothing to do
    }
};

#endif

class reactor::signal_pollfn final : public reactor::pollfn {
    reactor& _r;
public:
    signal_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() final override {
        return _r._signals.poll_signal();
    }
    virtual bool pure_poll() override final {
        return _r._signals.pure_poll_signal();
    }
    virtual bool try_enter_interrupt_mode() override {
        // Signals will interrupt our epoll_pwait() call, but
        // disable them now to avoid a signal between this point
        // and epoll_pwait()
        sigset_t block_all;
        sigfillset(&block_all);
        ::pthread_sigmask(SIG_SETMASK, &block_all, &_r._active_sigmask);
        if (poll()) {
            // raced already, and lost
            exit_interrupt_mode();
            return false;
        }
        return true;
    }
    virtual void exit_interrupt_mode() override final {
        ::pthread_sigmask(SIG_SETMASK, &_r._active_sigmask, nullptr);
    }
};

class reactor::batch_flush_pollfn final : public reactor::pollfn {
    reactor& _r;
public:
    batch_flush_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() final override {
        return _r.flush_tcp_batches();
    }
    virtual bool pure_poll() override final {
        return poll(); // actually performs work, but triggers no user continuations, so okay
    }
    virtual bool try_enter_interrupt_mode() override {
        // This is a passive poller, so if a previous poll
        // returned false (idle), there's no more work to do.
        return true;
    }
    virtual void exit_interrupt_mode() override final {
    }
};

class reactor::aio_batch_submit_pollfn final : public reactor::pollfn {
    reactor& _r;
public:
    aio_batch_submit_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() final override {
        return _r.flush_pending_aio();
    }
    virtual bool pure_poll() override final {
        return poll(); // actually performs work, but triggers no user continuations, so okay
    }
    virtual bool try_enter_interrupt_mode() override {
        // This is a passive poller, so if a previous poll
        // returned false (idle), there's no more work to do.
        return true;
    }
    virtual void exit_interrupt_mode() override final {
    }
};

class reactor::drain_cross_cpu_freelist_pollfn final : public reactor::pollfn {
public:
    virtual bool poll() final override {
        return memory::drain_cross_cpu_freelist();
    }
    virtual bool pure_poll() override final {
        return poll(); // actually performs work, but triggers no user continuations, so okay
    }
    virtual bool try_enter_interrupt_mode() override {
        // Other cpus can queue items for us to free; and they won't notify
        // us about them.  But it's okay to ignore those items, freeing them
        // doesn't have any side effects.
        //
        // We'll take care of those items when we wake up for another reason.
        return true;
    }
    virtual void exit_interrupt_mode() override final {
    }
};

class reactor::lowres_timer_pollfn final : public reactor::pollfn {
    reactor& _r;
    // A highres timer is implemented as a waking  signal; so
    // we arm one when we have a lowres timer during sleep, so
    // it can wake us up.
    timer<> _nearest_wakeup { [this] { _armed = false; } };
    bool _armed = false;
public:
    lowres_timer_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() final override {
        return _r.do_expire_lowres_timers();
    }
    virtual bool pure_poll() final override {
        return _r.do_check_lowres_timers();
    }
    virtual bool try_enter_interrupt_mode() override {
        // arm our highres timer so a signal will wake us up
        auto next = _r._lowres_next_timeout;
        if (next == lowres_clock::time_point()) {
            // no pending timers
            return true;
        }
        auto now = lowres_clock::now();
        if (next <= now) {
            // whoops, go back
            return false;
        }
        _nearest_wakeup.arm(next - now);
        _armed = true;
        return true;
    }
    virtual void exit_interrupt_mode() override final {
        if (_armed) {
            _nearest_wakeup.cancel();
            _armed = false;
        }
    }
};

class reactor::smp_pollfn final : public reactor::pollfn {
    reactor& _r;
public:
    smp_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() final override {
        return (smp::poll_queues() |
                alien::smp::poll_queues());
    }
    virtual bool pure_poll() final override {
        return (smp::pure_poll_queues() ||
                alien::smp::pure_poll_queues());
    }
    virtual bool try_enter_interrupt_mode() override {
        // systemwide_memory_barrier() is very slow if run concurrently,
        // so don't go to sleep if it is running now.
        _r._sleeping.store(true, std::memory_order_relaxed);
        bool barrier_done = try_systemwide_memory_barrier();
        if (!barrier_done) {
            _r._sleeping.store(false, std::memory_order_relaxed);
            return false;
        }
        if (poll()) {
            // raced
            _r._sleeping.store(false, std::memory_order_relaxed);
            return false;
        }
        return true;
    }
    virtual void exit_interrupt_mode() override final {
        _r._sleeping.store(false, std::memory_order_relaxed);
    }
};

class reactor::execution_stage_pollfn final : public reactor::pollfn {
    internal::execution_stage_manager& _esm;
public:
    execution_stage_pollfn() : _esm(internal::execution_stage_manager::get()) { }

    virtual bool poll() override {
        return _esm.flush();
    }
    virtual bool pure_poll() override {
        return _esm.poll();
    }
    virtual bool try_enter_interrupt_mode() override {
        // This is a passive poller, so if a previous poll
        // returned false (idle), there's no more work to do.
        return true;
    }
    virtual void exit_interrupt_mode() override { }
};

class reactor::syscall_pollfn final : public reactor::pollfn {
    reactor& _r;
public:
    syscall_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() final override {
        return _r._thread_pool.complete();
    }
    virtual bool pure_poll() override final {
        return poll(); // actually performs work, but triggers no user continuations, so okay
    }
    virtual bool try_enter_interrupt_mode() override {
        _r._thread_pool.enter_interrupt_mode();
        if (poll()) {
            // raced
            _r._thread_pool.exit_interrupt_mode();
            return false;
        }
        return true;
    }
    virtual void exit_interrupt_mode() override final {
        _r._thread_pool.exit_interrupt_mode();
    }
};


class reactor::epoll_pollfn final : public reactor::pollfn {
    reactor& _r;
public:
    epoll_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() final override {
        return _r.wait_and_process();
    }
    virtual bool pure_poll() override final {
        return poll(); // actually performs work, but triggers no user continuations, so okay
    }
    virtual bool try_enter_interrupt_mode() override {
        // Since we'll be sleeping in epoll, no need to do anything
        // for interrupt mode.
        return true;
    }
    virtual void exit_interrupt_mode() override final {
    }
};

void
reactor::wakeup() {
    uint64_t one = 1;
    ::write(_notify_eventfd.get(), &one, sizeof(one));
}

void reactor::start_aio_eventfd_loop() {
    if (!_aio_eventfd) {
        return;
    }
    future<> loop_done = repeat([this] {
        return _aio_eventfd->readable().then([this] {
            char garbage[8];
            ::read(_aio_eventfd->get_fd(), garbage, 8); // totally uninteresting
            return _stopping ? stop_iteration::yes : stop_iteration::no;
        });
    });
    // must use make_lw_shared, because at_exit expects a copyable function
    at_exit([loop_done = make_lw_shared(std::move(loop_done))] {
        return std::move(*loop_done);
    });
}

void reactor::stop_aio_eventfd_loop() {
    if (!_aio_eventfd) {
        return;
    }
    uint64_t one = 1;
    ::write(_aio_eventfd->get_fd(), &one, 8);
}

inline
bool
reactor::have_more_tasks() const {
    return _active_task_queues.size() + _activating_task_queues.size();
}

void reactor::insert_active_task_queue(task_queue* tq) {
    tq->_active = true;
    auto& atq = _active_task_queues;
    auto less = task_queue::indirect_compare();
    if (atq.empty() || less(atq.back(), tq)) {
        // Common case: idle->working
        // Common case: CPU intensive task queue going to the back
        atq.push_back(tq);
    } else {
        // Common case: newly activated queue preempting everything else
        atq.push_front(tq);
        // Less common case: newly activated queue behind something already active
        size_t i = 0;
        while (i + 1 != atq.size() && !less(atq[i], atq[i+1])) {
            std::swap(atq[i], atq[i+1]);
            ++i;
        }
    }
}

void
reactor::insert_activating_task_queues() {
    // Quadratic, but since we expect the common cases in insert_active_task_queue() to dominate, faster
    for (auto&& tq : _activating_task_queues) {
        insert_active_task_queue(tq);
    }
    _activating_task_queues.clear();
}

void
reactor::run_some_tasks() {
    if (!have_more_tasks()) {
        return;
    }
    sched_print("run_some_tasks: start");
    reset_preemption_monitor();

    sched_clock::time_point t_run_completed = std::chrono::steady_clock::now();
    STAP_PROBE(seastar, reactor_run_tasks_start);
    _cpu_stall_detector->start_task_run(t_run_completed);
    do {
        auto t_run_started = t_run_completed;
        insert_activating_task_queues();
        auto tq = _active_task_queues.front();
        _active_task_queues.pop_front();
        sched_print("running tq {} {}", (void*)tq, tq->_name);
        tq->_current = true;
        _last_vruntime = std::max(tq->_vruntime, _last_vruntime);
        run_tasks(*tq);
        tq->_current = false;
        t_run_completed = std::chrono::steady_clock::now();
        auto delta = t_run_completed - t_run_started;
        account_runtime(*tq, delta);
        sched_print("run complete ({} {}); time consumed {} usec; final vruntime {} empty {}",
                (void*)tq, tq->_name, delta / 1us, tq->_vruntime, tq->_q.empty());
        if (!tq->_q.empty()) {
            insert_active_task_queue(tq);
        } else {
            tq->_active = false;
        }
    } while (have_more_tasks() && !need_preempt());
    _cpu_stall_detector->end_task_run(t_run_completed);
    STAP_PROBE(seastar, reactor_run_tasks_end);
    *internal::current_scheduling_group_ptr() = default_scheduling_group(); // Prevent inheritance from last group run
    sched_print("run_some_tasks: end");
}

void
reactor::activate(task_queue& tq) {
    if (tq._active) {
        return;
    }
    sched_print("activating {} {}", (void*)&tq, tq._name);
    // If activate() was called, the task queue is likely network-bound or I/O bound, not CPU-bound. As
    // such its vruntime will be low, and it will have a large advantage over other task queues. Limit
    // the advantage so it doesn't dominate scheduling for a long time, in case it _does_ become CPU
    // bound later.
    //
    // FIXME: different scheduling groups have different sensitivity to jitter, take advantage
    if (_last_vruntime > tq._vruntime) {
        sched_print("tq {} {} losing vruntime {} due to sleep", (void*)&tq, tq._name, _last_vruntime - tq._vruntime);
    }
    tq._vruntime = std::max(_last_vruntime, tq._vruntime);
    _activating_task_queues.push_back(&tq);
}

void reactor::service_highres_timer() {
    complete_timers(_timers, _expired_timers, [this] {
        if (!_timers.empty()) {
            enable_timer(_timers.get_next_timeout());
        }
    });
}

int reactor::run() {
    auto signal_stack = install_signal_handler_stack();

    register_metrics();

    compat::optional<poller> io_poller = {};
    compat::optional<poller> aio_poller = {};
    compat::optional<poller> smp_poller = {};

    // I/O Performance greatly increases if the smp poller runs before the I/O poller. This is
    // because requests that were just added can be polled and processed by the I/O poller right
    // away.
    if (smp::count > 1) {
        smp_poller = poller(std::make_unique<smp_pollfn>(*this));
    }
    if (my_io_queues.size() > 0) {
#ifndef HAVE_OSV
        io_poller = poller(std::make_unique<io_pollfn>(*this));
#endif
        aio_poller = poller(std::make_unique<aio_batch_submit_pollfn>(*this));
    }

    poller sig_poller(std::make_unique<signal_pollfn>(*this));
    poller batch_flush_poller(std::make_unique<batch_flush_pollfn>(*this));
    poller execution_stage_poller(std::make_unique<execution_stage_pollfn>());

    start_aio_eventfd_loop();

    if (_id == 0) {
       if (_handle_sigint) {
          _signals.handle_signal_once(SIGINT, [this] { stop(); });
       }
       _signals.handle_signal_once(SIGTERM, [this] { stop(); });
    }

    _cpu_started.wait(smp::count).then([this] {
        _network_stack->initialize().then([this] {
            _start_promise.set_value();
        });
    });
    _network_stack_ready_promise.get_future().then([this] (std::unique_ptr<network_stack> stack) {
        _network_stack = std::move(stack);
        for (unsigned c = 0; c < smp::count; c++) {
            smp::submit_to(c, [] {
                    engine()._cpu_started.signal();
            });
        }
    });

    poller syscall_poller(std::make_unique<syscall_pollfn>(*this));

    poller drain_cross_cpu_freelist(std::make_unique<drain_cross_cpu_freelist_pollfn>());

    poller expire_lowres_timers(std::make_unique<lowres_timer_pollfn>(*this));

    using namespace std::chrono_literals;
    timer<lowres_clock> load_timer;
    auto last_idle = _total_idle;
    auto idle_start = sched_clock::now(), idle_end = idle_start;
    load_timer.set_callback([this, &last_idle, &idle_start, &idle_end] () mutable {
        _total_idle += idle_end - idle_start;
        auto load = double((_total_idle - last_idle).count()) / double(std::chrono::duration_cast<sched_clock::duration>(1s).count());
        last_idle = _total_idle;
        load = std::min(load, 1.0);
        idle_start = idle_end;
        _loads.push_front(load);
        if (_loads.size() > 5) {
            auto drop = _loads.back();
            _loads.pop_back();
            _load -= (drop/5);
        }
        _load += (load/5);
    });
    load_timer.arm_periodic(1s);

    itimerspec its = seastar::posix::to_relative_itimerspec(_task_quota, _task_quota);
    _task_quota_timer.timerfd_settime(0, its);
    auto& task_quote_itimerspec = its;

    struct sigaction sa_block_notifier = {};
    sa_block_notifier.sa_handler = &reactor::block_notifier;
    sa_block_notifier.sa_flags = SA_RESTART;
    auto r = sigaction(cpu_stall_detector::signal_number(), &sa_block_notifier, nullptr);
    assert(r == 0);

    bool idle = false;

    std::function<bool()> check_for_work = [this] () {
        return poll_once() || have_more_tasks() || seastar::thread::try_run_one_yielded_thread();
    };
    std::function<bool()> pure_check_for_work = [this] () {
        return pure_poll_once() || have_more_tasks() || seastar::thread::try_run_one_yielded_thread();
    };
    while (true) {
        run_some_tasks();
        if (_stopped) {
            load_timer.cancel();
            // Final tasks may include sending the last response to cpu 0, so run them
            while (have_more_tasks()) {
                run_some_tasks();
            }
            while (!_at_destroy_tasks->_q.empty()) {
                run_tasks(*_at_destroy_tasks);
            }
            smp::arrive_at_event_loop_end();
            if (_id == 0) {
                smp::join_all();
            }
            break;
        }

        increment_nonatomically(_polls);

        if (check_for_work()) {
            if (idle) {
                _total_idle += idle_end - idle_start;
                account_idle(idle_end - idle_start);
                idle_start = idle_end;
                idle = false;
            }
        } else {
            idle_end = sched_clock::now();
            if (!idle) {
                idle_start = idle_end;
                idle = true;
            }
            bool go_to_sleep = true;
            try {
                // we can't run check_for_work(), because that can run tasks in the context
                // of the idle handler which change its state, without the idle handler expecting
                // it.  So run pure_check_for_work() instead.
                auto handler_result = _idle_cpu_handler(pure_check_for_work);
                go_to_sleep = handler_result == idle_cpu_handler_result::no_more_work;
            } catch (...) {
                report_exception("Exception while running idle cpu handler", std::current_exception());
            }
            if (go_to_sleep) {
                internal::cpu_relax();
                if (idle_end - idle_start > _max_poll_time) {
                    // Turn off the task quota timer to avoid spurious wakeups
                    struct itimerspec zero_itimerspec = {};
                    _task_quota_timer.timerfd_settime(0, zero_itimerspec);
                    auto start_sleep = sched_clock::now();
                    _cpu_stall_detector->start_sleep();
                    sleep();
                    _cpu_stall_detector->end_sleep();
                    // We may have slept for a while, so freshen idle_end
                    idle_end = sched_clock::now();
                    _total_sleep += idle_end - start_sleep;
                    _task_quota_timer.timerfd_settime(0, task_quote_itimerspec);
                }
            } else {
                // We previously ran pure_check_for_work(), might not actually have performed
                // any work.
                check_for_work();
            }
        }
    }
    // To prevent ordering issues from rising, destroy the I/O queue explicitly at this point.
    // This is needed because the reactor is destroyed from the thread_local destructors. If
    // the I/O queue happens to use any other infrastructure that is also kept this way (for
    // instance, collectd), we will not have any way to guarantee who is destroyed first.
    my_io_queues.clear();
    return _return;
}

void
reactor::sleep() {
    for (auto i = _pollers.begin(); i != _pollers.end(); ++i) {
        auto ok = (*i)->try_enter_interrupt_mode();
        if (!ok) {
            while (i != _pollers.begin()) {
                (*--i)->exit_interrupt_mode();
            }
            return;
        }
    }
    wait_and_process(-1, &_active_sigmask);
    for (auto i = _pollers.rbegin(); i != _pollers.rend(); ++i) {
        (*i)->exit_interrupt_mode();
    }
}

void
reactor::start_epoll() {
    if (!_epoll_poller) {
        _epoll_poller = poller(std::make_unique<epoll_pollfn>(*this));
    }
}

bool
reactor::poll_once() {
    bool work = false;
    for (auto c : _pollers) {
        work |= c->poll();
    }

    return work;
}

bool
reactor::pure_poll_once() {
    for (auto c : _pollers) {
        if (c->pure_poll()) {
            return true;
        }
    }
    return false;
}

class reactor::poller::registration_task : public task {
private:
    poller* _p;
public:
    explicit registration_task(poller* p) : _p(p) {}
    virtual void run_and_dispose() noexcept override {
        if (_p) {
            engine().register_poller(_p->_pollfn.get());
            _p->_registration_task = nullptr;
        }
        delete this;
    }
    void cancel() {
        _p = nullptr;
    }
    void moved(poller* p) {
        _p = p;
    }
};

class reactor::poller::deregistration_task : public task {
private:
    std::unique_ptr<pollfn> _p;
public:
    explicit deregistration_task(std::unique_ptr<pollfn>&& p) : _p(std::move(p)) {}
    virtual void run_and_dispose() noexcept override {
        engine().unregister_poller(_p.get());
        delete this;
    }
};

void reactor::register_poller(pollfn* p) {
    _pollers.push_back(p);
}

void reactor::unregister_poller(pollfn* p) {
    _pollers.erase(std::find(_pollers.begin(), _pollers.end(), p));
}

void reactor::replace_poller(pollfn* old, pollfn* neww) {
    std::replace(_pollers.begin(), _pollers.end(), old, neww);
}

reactor::poller::poller(poller&& x)
        : _pollfn(std::move(x._pollfn)), _registration_task(x._registration_task) {
    if (_pollfn && _registration_task) {
        _registration_task->moved(this);
    }
}

reactor::poller&
reactor::poller::operator=(poller&& x) {
    if (this != &x) {
        this->~poller();
        new (this) poller(std::move(x));
    }
    return *this;
}

void
reactor::poller::do_register() {
    // We can't just insert a poller into reactor::_pollers, because we
    // may be running inside a poller ourselves, and so in the middle of
    // iterating reactor::_pollers itself.  So we schedule a task to add
    // the poller instead.
    auto task = std::make_unique<registration_task>(this);
    auto tmp = task.get();
    engine().add_task(std::move(task));
    _registration_task = tmp;
}

reactor::poller::~poller() {
    // We can't just remove the poller from reactor::_pollers, because we
    // may be running inside a poller ourselves, and so in the middle of
    // iterating reactor::_pollers itself.  So we schedule a task to remove
    // the poller instead.
    //
    // Since we don't want to call the poller after we exit the destructor,
    // we replace it atomically with another one, and schedule a task to
    // delete the replacement.
    if (_pollfn) {
        if (_registration_task) {
            // not added yet, so don't do it at all.
            _registration_task->cancel();
        } else {
            auto dummy = make_pollfn([] { return false; });
            auto dummy_p = dummy.get();
            auto task = std::make_unique<deregistration_task>(std::move(dummy));
            engine().add_task(std::move(task));
            engine().replace_poller(_pollfn.get(), dummy_p);
        }
    }
}

bool
reactor_backend_epoll::wait_and_process(int timeout, const sigset_t* active_sigmask) {
    std::array<epoll_event, 128> eevt;
    int nr = ::epoll_pwait(_epollfd.get(), eevt.data(), eevt.size(), timeout, active_sigmask);
    if (nr == -1 && errno == EINTR) {
        return false; // gdb can cause this
    }
    assert(nr != -1);
    for (int i = 0; i < nr; ++i) {
        auto& evt = eevt[i];
        auto pfd = reinterpret_cast<pollable_fd_state*>(evt.data.ptr);
        if (!pfd) {
            char dummy[8];
            _r->_notify_eventfd.read(dummy, 8);
            continue;
        }
        auto events = evt.events & (EPOLLIN | EPOLLOUT);
        auto events_to_remove = events & ~pfd->events_requested;
        if (pfd->events_rw) {
            // accept() signals normal completions via EPOLLIN, but errors (due to shutdown())
            // via EPOLLOUT|EPOLLHUP, so we have to wait for both EPOLLIN and EPOLLOUT with the
            // same future
            complete_epoll_event(*pfd, &pollable_fd_state::pollin, events, EPOLLIN|EPOLLOUT);
        } else {
            // Normal processing where EPOLLIN and EPOLLOUT are waited for via different
            // futures.
            complete_epoll_event(*pfd, &pollable_fd_state::pollin, events, EPOLLIN);
            complete_epoll_event(*pfd, &pollable_fd_state::pollout, events, EPOLLOUT);
        }
        if (events_to_remove) {
            pfd->events_epoll &= ~events_to_remove;
            evt.events = pfd->events_epoll;
            auto op = evt.events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            ::epoll_ctl(_epollfd.get(), op, pfd->fd.get(), &evt);
        }
    }
    return nr;
}

syscall_work_queue::syscall_work_queue()
    : _pending()
    , _completed()
    , _start_eventfd(0) {
}

void syscall_work_queue::submit_item(std::unique_ptr<syscall_work_queue::work_item> item) {
    _queue_has_room.wait().then([this, item = std::move(item)] () mutable {
        _pending.push(item.release());
        _start_eventfd.signal(1);
    });
}

unsigned syscall_work_queue::complete() {
    std::array<work_item*, queue_length> tmp_buf;
    auto end = tmp_buf.data();
    auto nr = _completed.consume_all([&] (work_item* wi) {
        *end++ = wi;
    });
    for (auto p = tmp_buf.data(); p != end; ++p) {
        auto wi = *p;
        wi->complete();
        delete wi;
    }
    _queue_has_room.signal(nr);
    return nr;
}

smp_message_queue::smp_message_queue(reactor* from, reactor* to)
    : _pending(to)
    , _completed(from)
{
}

smp_message_queue::~smp_message_queue()
{
    if (_pending.remote != _completed.remote) {
        _tx.a.~aa();
    }
}

void smp_message_queue::stop() {
    _metrics.clear();
}

void smp_message_queue::move_pending() {
    auto begin = _tx.a.pending_fifo.cbegin();
    auto end = _tx.a.pending_fifo.cend();
    end = _pending.push(begin, end);
    if (begin == end) {
        return;
    }
    auto nr = end - begin;
    _pending.maybe_wakeup();
    _tx.a.pending_fifo.erase(begin, end);
    _current_queue_length += nr;
    _last_snt_batch = nr;
    _sent += nr;
}

bool smp_message_queue::pure_poll_tx() const {
    // can't use read_available(), not available on older boost
    // empty() is not const, so need const_cast.
    return !const_cast<lf_queue&>(_completed).empty();
}

void smp_message_queue::submit_item(std::unique_ptr<smp_message_queue::work_item> item) {
    _tx.a.pending_fifo.push_back(item.get());
    item.release();
    if (_tx.a.pending_fifo.size() >= batch_size) {
        move_pending();
    }
}

void smp_message_queue::respond(work_item* item) {
    _completed_fifo.push_back(item);
    if (_completed_fifo.size() >= batch_size || engine()._stopped) {
        flush_response_batch();
    }
}

void smp_message_queue::flush_response_batch() {
    if (!_completed_fifo.empty()) {
        auto begin = _completed_fifo.cbegin();
        auto end = _completed_fifo.cend();
        end = _completed.push(begin, end);
        if (begin == end) {
            return;
        }
        _completed.maybe_wakeup();
        _completed_fifo.erase(begin, end);
    }
}

bool smp_message_queue::has_unflushed_responses() const {
    return !_completed_fifo.empty();
}

bool smp_message_queue::pure_poll_rx() const {
    // can't use read_available(), not available on older boost
    // empty() is not const, so need const_cast.
    return !const_cast<lf_queue&>(_pending).empty();
}

void
smp_message_queue::lf_queue::maybe_wakeup() {
    // Called after lf_queue_base::push().
    //
    // This is read-after-write, which wants memory_order_seq_cst,
    // but we insert that barrier using systemwide_memory_barrier()
    // because seq_cst is so expensive.
    //
    // However, we do need a compiler barrier:
    std::atomic_signal_fence(std::memory_order_seq_cst);
    if (remote->_sleeping.load(std::memory_order_relaxed)) {
        // We are free to clear it, because we're sending a signal now
        remote->_sleeping.store(false, std::memory_order_relaxed);
        remote->wakeup();
    }
}

template<size_t PrefetchCnt, typename Func>
size_t smp_message_queue::process_queue(lf_queue& q, Func process) {
    // copy batch to local memory in order to minimize
    // time in which cross-cpu data is accessed
    work_item* items[queue_length + PrefetchCnt];
    work_item* wi;
    if (!q.pop(wi))
        return 0;
    // start prefetching first item before popping the rest to overlap memory
    // access with potential cache miss the second pop may cause
    prefetch<2>(wi);
    auto nr = q.pop(items);
    std::fill(std::begin(items) + nr, std::begin(items) + nr + PrefetchCnt, nr ? items[nr - 1] : wi);
    unsigned i = 0;
    do {
        prefetch_n<2>(std::begin(items) + i, std::begin(items) + i + PrefetchCnt);
        process(wi);
        wi = items[i++];
    } while(i <= nr);

    return nr + 1;
}

size_t smp_message_queue::process_completions() {
    auto nr = process_queue<prefetch_cnt*2>(_completed, [] (work_item* wi) {
        wi->complete();
        delete wi;
    });
    _current_queue_length -= nr;
    _compl += nr;
    _last_cmpl_batch = nr;

    return nr;
}

void smp_message_queue::flush_request_batch() {
    if (!_tx.a.pending_fifo.empty()) {
        move_pending();
    }
}

size_t smp_message_queue::process_incoming() {
    auto nr = process_queue<prefetch_cnt>(_pending, [] (work_item* wi) {
        wi->process();
    });
    _received += nr;
    _last_rcv_batch = nr;
    return nr;
}

void smp_message_queue::start(unsigned cpuid) {
    _tx.init();
    namespace sm = seastar::metrics;
    char instance[10];
    std::snprintf(instance, sizeof(instance), "%u-%u", engine().cpu_id(), cpuid);
    _metrics.add_group("smp", {
            // queue_length     value:GAUGE:0:U
            // Absolute value of num packets in last tx batch.
            sm::make_queue_length("send_batch_queue_length", _last_snt_batch, sm::description("Current send batch queue length"), {sm::shard_label(instance)})(sm::metric_disabled),
            sm::make_queue_length("receive_batch_queue_length", _last_rcv_batch, sm::description("Current receive batch queue length"), {sm::shard_label(instance)})(sm::metric_disabled),
            sm::make_queue_length("complete_batch_queue_length", _last_cmpl_batch, sm::description("Current complete batch queue length"), {sm::shard_label(instance)})(sm::metric_disabled),
            sm::make_queue_length("send_queue_length", _current_queue_length, sm::description("Current send queue length"), {sm::shard_label(instance)})(sm::metric_disabled),
            // total_operations value:DERIVE:0:U
            sm::make_derive("total_received_messages", _received, sm::description("Total number of received messages"), {sm::shard_label(instance)})(sm::metric_disabled),
            // total_operations value:DERIVE:0:U
            sm::make_derive("total_sent_messages", _sent, sm::description("Total number of sent messages"), {sm::shard_label(instance)})(sm::metric_disabled),
            // total_operations value:DERIVE:0:U
            sm::make_derive("total_completed_messages", _compl, sm::description("Total number of messages completed"), {sm::shard_label(instance)})(sm::metric_disabled)
    });
}

/* not yet implemented for OSv. TODO: do the notification like we do class smp. */
#ifndef HAVE_OSV
thread_pool::thread_pool(reactor* r, sstring name) : _reactor(r), _worker_thread([this, name] { work(name); }) {
}

void thread_pool::work(sstring name) {
    pthread_setname_np(pthread_self(), name.c_str());
    sigset_t mask;
    sigfillset(&mask);
    auto r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
    throw_pthread_error(r);
    std::array<syscall_work_queue::work_item*, syscall_work_queue::queue_length> tmp_buf;
    while (true) {
        uint64_t count;
        auto r = ::read(inter_thread_wq._start_eventfd.get_read_fd(), &count, sizeof(count));
        assert(r == sizeof(count));
        if (_stopped.load(std::memory_order_relaxed)) {
            break;
        }
        auto end = tmp_buf.data();
        inter_thread_wq._pending.consume_all([&] (syscall_work_queue::work_item* wi) {
            *end++ = wi;
        });
        for (auto p = tmp_buf.data(); p != end; ++p) {
            auto wi = *p;
            wi->process();
            inter_thread_wq._completed.push(wi);
        }
        if (_main_thread_idle.load(std::memory_order_seq_cst)) {
            uint64_t one = 1;
            ::write(_reactor->_notify_eventfd.get(), &one, 8);
        }
    }
}

thread_pool::~thread_pool() {
    _stopped.store(true, std::memory_order_relaxed);
    inter_thread_wq._start_eventfd.signal(1);
    _worker_thread.join();
}
#endif

readable_eventfd writeable_eventfd::read_side() {
    return readable_eventfd(_fd.dup());
}

file_desc writeable_eventfd::try_create_eventfd(size_t initial) {
    assert(size_t(int(initial)) == initial);
    return file_desc::eventfd(initial, EFD_CLOEXEC);
}

void writeable_eventfd::signal(size_t count) {
    uint64_t c = count;
    auto r = _fd.write(&c, sizeof(c));
    assert(r == sizeof(c));
}

writeable_eventfd readable_eventfd::write_side() {
    return writeable_eventfd(_fd.get_file_desc().dup());
}

file_desc readable_eventfd::try_create_eventfd(size_t initial) {
    assert(size_t(int(initial)) == initial);
    return file_desc::eventfd(initial, EFD_CLOEXEC | EFD_NONBLOCK);
}

future<size_t> readable_eventfd::wait() {
    return engine().readable(*_fd._s).then([this] {
        uint64_t count;
        int r = ::read(_fd.get_fd(), &count, sizeof(count));
        assert(r == sizeof(count));
        return make_ready_future<size_t>(count);
    });
}

void schedule(std::unique_ptr<task> t) {
    engine().add_task(std::move(t));
}

void schedule_urgent(std::unique_ptr<task> t) {
    engine().add_urgent_task(std::move(t));
}

}

bool operator==(const ::sockaddr_in a, const ::sockaddr_in b) {
    return (a.sin_addr.s_addr == b.sin_addr.s_addr) && (a.sin_port == b.sin_port);
}

namespace seastar {

void network_stack_registry::register_stack(sstring name,
        boost::program_options::options_description opts,
        std::function<future<std::unique_ptr<network_stack>> (options opts)> create, bool make_default) {
    _map()[name] = std::move(create);
    options_description().add(opts);
    if (make_default) {
        _default() = name;
    }
}

void register_network_stack(sstring name, boost::program_options::options_description opts,
    std::function<future<std::unique_ptr<network_stack>>(boost::program_options::variables_map)>
        create,
    bool make_default) {
    return network_stack_registry::register_stack(
        std::move(name), std::move(opts), std::move(create), make_default);
}

sstring network_stack_registry::default_stack() {
    return _default();
}

std::vector<sstring> network_stack_registry::list() {
    std::vector<sstring> ret;
    for (auto&& ns : _map()) {
        ret.push_back(ns.first);
    }
    return ret;
}

future<std::unique_ptr<network_stack>>
network_stack_registry::create(options opts) {
    return create(_default(), opts);
}

future<std::unique_ptr<network_stack>>
network_stack_registry::create(sstring name, options opts) {
    if (!_map().count(name)) {
        throw std::runtime_error(format("network stack {} not registered", name));
    }
    return _map()[name](opts);
}

boost::program_options::options_description
reactor::get_options_description(std::chrono::duration<double> default_task_quota) {
    namespace bpo = boost::program_options;
    bpo::options_description opts("Core options");
    auto net_stack_names = network_stack_registry::list();
    opts.add_options()
        ("network-stack", bpo::value<std::string>(),
                format("select network stack (valid values: {})",
                        format_separated(net_stack_names.begin(), net_stack_names.end(), ", ")).c_str())
        ("no-handle-interrupt", "ignore SIGINT (for gdb)")
        ("poll-mode", "poll continuously (100% cpu use)")
        ("idle-poll-time-us", bpo::value<unsigned>()->default_value(calculate_poll_time() / 1us),
                "idle polling time in microseconds (reduce for overprovisioned environments or laptops)")
        ("poll-aio", bpo::value<bool>()->default_value(true),
                "busy-poll for disk I/O (reduces latency and increases throughput)")
        ("task-quota-ms", bpo::value<double>()->default_value(default_task_quota / 1ms), "Max time (ms) between polls")
        ("max-task-backlog", bpo::value<unsigned>()->default_value(1000), "Maximum number of task backlog to allow; above this we ignore I/O")
        ("blocked-reactor-notify-ms", bpo::value<unsigned>()->default_value(2000), "threshold in miliseconds over which the reactor is considered blocked if no progress is made")
        ("blocked-reactor-reports-per-minute", bpo::value<unsigned>()->default_value(5), "Maximum number of backtraces reported by stall detector per minute")
        ("relaxed-dma", "allow using buffered I/O if DMA is not available (reduces performance)")
        ("unsafe-bypass-fsync", bpo::value<bool>()->default_value(false), "Bypass fsync(), may result in data loss. Use for testing on consumer drives")
        ("overprovisioned", "run in an overprovisioned environment (such as docker or a laptop); equivalent to --idle-poll-time-us 0 --thread-affinity 0 --poll-aio 0")
        ("abort-on-seastar-bad-alloc", "abort when seastar allocator cannot allocate memory")
        ("force-aio-syscalls", bpo::value<bool>()->default_value(false),
                "Force io_getevents(2) to issue a system call, instead of bypassing the kernel when possible."
                " This makes strace output more useful, but slows down the application")
        ("reactor-backend", bpo::value<reactor_backend_selector>()->default_value(reactor_backend_selector::default_backend()),
                format("Internal reactor implementation ({})", reactor_backend_selector::available()).c_str())
#ifdef SEASTAR_HEAPPROF
        ("heapprof", "enable seastar heap profiling")
#endif
        ;
    opts.add(network_stack_registry::options_description());
    return opts;
}

boost::program_options::options_description
smp::get_options_description()
{
    namespace bpo = boost::program_options;
    bpo::options_description opts("SMP options");
    opts.add_options()
        ("smp,c", bpo::value<unsigned>(), "number of threads (default: one per CPU)")
        ("cpuset", bpo::value<cpuset_bpo_wrapper>(), "CPUs to use (in cpuset(7) format; default: all))")
        ("memory,m", bpo::value<std::string>(), "memory to use, in bytes (ex: 4G) (default: all)")
        ("reserve-memory", bpo::value<std::string>(), "memory reserved to OS (if --memory not specified)")
        ("hugepages", bpo::value<std::string>(), "path to accessible hugetlbfs mount (typically /dev/hugepages/something)")
        ("lock-memory", bpo::value<bool>(), "lock all memory (prevents swapping)")
        ("thread-affinity", bpo::value<bool>()->default_value(true), "pin threads to their cpus (disable for overprovisioning)")
#ifdef SEASTAR_HAVE_HWLOC
        ("num-io-queues", bpo::value<unsigned>(), "Number of IO queues. Each IO unit will be responsible for a fraction of the IO requests. Defaults to the number of threads")
        ("max-io-requests", bpo::value<unsigned>(), "Maximum amount of concurrent requests to be sent to the disk. Defaults to 128 times the number of IO queues")
#else
        ("max-io-requests", bpo::value<unsigned>(), "Maximum amount of concurrent requests to be sent to the disk. Defaults to 128 times the number of processors")
#endif
        ("io-properties-file", bpo::value<std::string>(), "path to a YAML file describing the characteristics of the I/O Subsystem")
        ("io-properties", bpo::value<std::string>(), "a YAML string describing the characteristics of the I/O Subsystem")
        ("mbind", bpo::value<bool>()->default_value(true), "enable mbind")
#ifndef SEASTAR_NO_EXCEPTION_HACK
        ("enable-glibc-exception-scaling-workaround", bpo::value<bool>()->default_value(true), "enable workaround for glibc/gcc c++ exception scalablity problem")
#endif
        ;
    return opts;
}

thread_local scollectd::impl scollectd_impl;

scollectd::impl & scollectd::get_impl() {
    return scollectd_impl;
}

struct reactor_deleter {
    void operator()(reactor* p) {
        p->~reactor();
        free(p);
    }
};

thread_local std::unique_ptr<reactor, reactor_deleter> reactor_holder;

std::vector<posix_thread> smp::_threads;
std::vector<std::function<void ()>> smp::_thread_loops;
compat::optional<boost::barrier> smp::_all_event_loops_done;
std::vector<reactor*> smp::_reactors;
std::unique_ptr<smp_message_queue*[], smp::qs_deleter> smp::_qs;
std::thread::id smp::_tmain;
unsigned smp::count = 1;
bool smp::_using_dpdk;

void smp::start_all_queues()
{
    for (unsigned c = 0; c < count; c++) {
        if (c != engine().cpu_id()) {
            _qs[c][engine().cpu_id()].start(c);
        }
    }
    alien::smp::_qs[engine().cpu_id()].start();
}

#ifdef SEASTAR_HAVE_DPDK

int dpdk_thread_adaptor(void* f)
{
    (*static_cast<std::function<void ()>*>(f))();
    return 0;
}

#endif

void smp::join_all()
{
#ifdef SEASTAR_HAVE_DPDK
    if (_using_dpdk) {
        rte_eal_mp_wait_lcore();
        return;
    }
#endif
    for (auto&& t: smp::_threads) {
        t.join();
    }
}

void smp::pin(unsigned cpu_id) {
    if (_using_dpdk) {
        // dpdk does its own pinning
        return;
    }
    pin_this_thread(cpu_id);
}

void smp::arrive_at_event_loop_end() {
    if (_all_event_loops_done) {
        _all_event_loops_done->wait();
    }
}

void smp::allocate_reactor(unsigned id, reactor_backend_selector rbs) {
    assert(!reactor_holder);

    // we cannot just write "local_engin = new reactor" since reactor's constructor
    // uses local_engine
    void *buf;
    int r = posix_memalign(&buf, cache_line_size, sizeof(reactor));
    assert(r == 0);
    local_engine = reinterpret_cast<reactor*>(buf);
    new (buf) reactor(id, std::move(rbs));
    reactor_holder.reset(local_engine);
}

void smp::cleanup() {
    smp::_threads = std::vector<posix_thread>();
    _thread_loops.clear();
}

void smp::cleanup_cpu() {
    size_t cpuid = engine().cpu_id();

    if (_qs) {
        for(unsigned i = 0; i < smp::count; i++) {
            _qs[i][cpuid].stop();
        }
    }
    if (alien::smp::_qs) {
        alien::smp::_qs[cpuid].stop();
    }
}

void smp::create_thread(std::function<void ()> thread_loop) {
    if (_using_dpdk) {
        _thread_loops.push_back(std::move(thread_loop));
    } else {
        _threads.emplace_back(std::move(thread_loop));
    }
}

// Installs handler for Signal which ensures that Func is invoked only once
// in the whole program and that after it is invoked the default handler is restored.
template<int Signal, void(*Func)()>
void install_oneshot_signal_handler() {
    static bool handled = false;
    static util::spinlock lock;

    struct sigaction sa;
    sa.sa_sigaction = [](int sig, siginfo_t *info, void *p) {
        std::lock_guard<util::spinlock> g(lock);
        if (!handled) {
            handled = true;
            Func();
            signal(sig, SIG_DFL);
        }
    };
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    if (Signal == SIGSEGV) {
        sa.sa_flags |= SA_ONSTACK;
    }
    auto r = ::sigaction(Signal, &sa, nullptr);
    throw_system_error_on(r == -1);
}

static void sigsegv_action() noexcept {
    print_with_backtrace("Segmentation fault");
}

static void sigabrt_action() noexcept {
    print_with_backtrace("Aborting");
}

void smp::qs_deleter::operator()(smp_message_queue** qs) const {
    for (unsigned i = 0; i < smp::count; i++) {
        for (unsigned j = 0; j < smp::count; j++) {
            qs[i][j].~smp_message_queue();
        }
        ::operator delete[](qs[i]);
    }
    delete[](qs);
}

class disk_config_params {
private:
    unsigned _num_io_queues = 0;
    compat::optional<unsigned> _capacity;
    std::unordered_map<dev_t, mountpoint_params> _mountpoints;
    std::chrono::duration<double> _latency_goal;

public:
    uint64_t per_io_queue(uint64_t qty, dev_t devid) const {
        const mountpoint_params& p = _mountpoints.at(devid);
        return std::max(qty / p.num_io_queues, 1ul);
    }

    unsigned num_io_queues(dev_t devid) const {
        const mountpoint_params& p = _mountpoints.at(devid);
        return p.num_io_queues;
    }

    std::chrono::duration<double> latency_goal() const {
        return _latency_goal;
    }

    void parse_config(boost::program_options::variables_map& configuration) {
        seastar_logger.debug("smp::count: {}", smp::count);
        _latency_goal = std::chrono::duration_cast<std::chrono::duration<double>>(configuration["task-quota-ms"].as<double>() * 1.5 * 1ms);
        seastar_logger.debug("latency_goal: {}", latency_goal().count());

        if (configuration.count("max-io-requests")) {
            _capacity = configuration["max-io-requests"].as<unsigned>();
        }

        if (configuration.count("num-io-queues")) {
            _num_io_queues = configuration["num-io-queues"].as<unsigned>();
            if (!_num_io_queues) {
                throw std::runtime_error("num-io-queues must be greater than zero");
            }
        }
        if (configuration.count("io-properties-file") && configuration.count("io-properties")) {
            throw std::runtime_error("Both io-properties and io-properties-file specified. Don't know which to trust!");
        }

        compat::optional<YAML::Node> doc;
        if (configuration.count("io-properties-file")) {
            doc = YAML::LoadFile(configuration["io-properties-file"].as<std::string>());
        } else if (configuration.count("io-properties")) {
            doc = YAML::Load(configuration["io-properties"].as<std::string>());
        }

        if (doc) {
            static constexpr unsigned task_quotas_in_default_latency_goal = 3;
            unsigned auto_num_io_queues = smp::count;

            for (auto&& section : *doc) {
                auto sec_name = section.first.as<std::string>();
                if (sec_name != "disks") {
                    throw std::runtime_error(fmt::format("While parsing I/O options: section {} currently unsupported.", sec_name));
                }
                auto disks = section.second.as<std::vector<mountpoint_params>>();
                for (auto& d : disks) {
                    struct ::stat buf;
                    auto ret = stat(d.mountpoint.c_str(), &buf);
                    if (ret < 0) {
                        throw std::runtime_error(fmt::format("Couldn't stat {}", d.mountpoint));
                    }
                    if (_mountpoints.count(buf.st_dev)) {
                        throw std::runtime_error(fmt::format("Mountpoint {} already configured", d.mountpoint));
                    }
                    if (_mountpoints.size() >= reactor::max_queues) {
                        throw std::runtime_error(fmt::format("Configured number of queues {} is larger than the maximum {}",
                                                 _mountpoints.size(), reactor::max_queues));
                    }

                    // Ideally we wouldn't have I/O Queues and would dispatch from every shard (https://github.com/scylladb/seastar/issues/485)
                    // While we don't do that, we'll just be conservative and try to recommend values of I/O Queues that are close to what we
                    // suggested before the I/O Scheduler rework. The I/O Scheduler has traditionally tried to make sure that each queue would have
                    // at least 4 requests in depth, and all its requests were 4kB in size. Therefore, try to arrange the I/O Queues so that we would
                    // end up in the same situation here (that's where the 4 comes from).
                    //
                    // For the bandwidth limit, we want that to be 4 * 4096, so each I/O Queue has the same bandwidth as before.
                    if (!_num_io_queues) {
                        unsigned dev_io_queues = smp::count;
                        dev_io_queues = std::min(dev_io_queues, unsigned((task_quotas_in_default_latency_goal * d.write_req_rate * latency_goal().count()) / 4));
                        dev_io_queues = std::min(dev_io_queues, unsigned((task_quotas_in_default_latency_goal * d.write_bytes_rate * latency_goal().count()) / (4 * 4096)));
                        dev_io_queues = std::max(dev_io_queues, 1u);
                        seastar_logger.debug("dev_io_queues: {}", dev_io_queues);
                        d.num_io_queues = dev_io_queues;
                        auto_num_io_queues = std::min(auto_num_io_queues, dev_io_queues);
                    } else {
                        d.num_io_queues = _num_io_queues;
                    }

                    seastar_logger.debug("dev_id: {} mountpoint: {}", buf.st_dev, d.mountpoint);
                    _mountpoints.emplace(buf.st_dev, d);
                }
            }
            if (!_num_io_queues) {
                _num_io_queues = auto_num_io_queues;
            }
        } else if (!_num_io_queues) {
            _num_io_queues = smp::count;
        }

        // Placeholder for unconfigured disks.
        mountpoint_params d = {};
        d.num_io_queues = _num_io_queues;
        seastar_logger.debug("num_io_queues: {}", d.num_io_queues);
        _mountpoints.emplace(0, d);
    }

    struct io_queue::config generate_config(dev_t devid) const {
        seastar_logger.debug("generate_config dev_id: {}", devid);
        const mountpoint_params& p = _mountpoints.at(devid);
        struct io_queue::config cfg;
        uint64_t max_bandwidth = std::max(p.read_bytes_rate, p.write_bytes_rate);
        uint64_t max_iops = std::max(p.read_req_rate, p.write_req_rate);

        if (!_capacity) {
            cfg.disk_bytes_write_to_read_multiplier = (io_queue::read_request_base_count * p.read_bytes_rate) / p.write_bytes_rate;
            cfg.disk_req_write_to_read_multiplier = (io_queue::read_request_base_count * p.read_req_rate) / p.write_req_rate;
            if (max_bandwidth != std::numeric_limits<uint64_t>::max()) {
                cfg.max_bytes_count = io_queue::read_request_base_count * per_io_queue(max_bandwidth * latency_goal().count(), devid);
            }
            if (max_iops != std::numeric_limits<uint64_t>::max()) {
                cfg.max_req_count = io_queue::read_request_base_count * per_io_queue(max_iops * latency_goal().count(), devid);
            }
            cfg.mountpoint = p.mountpoint;
        } else {
            cfg.capacity = per_io_queue(*_capacity, 0);
            cfg.disk_bytes_write_to_read_multiplier = 1;
            cfg.disk_req_write_to_read_multiplier = 1;
        }
        return cfg;
    }

    auto device_ids() {
        return boost::adaptors::keys(_mountpoints);
    }
};

static void register_network_stacks() {
    register_posix_stack();
    register_native_stack();
}

void smp::configure(boost::program_options::variables_map configuration)
{
    register_network_stacks();
#ifndef SEASTAR_NO_EXCEPTION_HACK
    if (configuration["enable-glibc-exception-scaling-workaround"].as<bool>()) {
        init_phdr_cache();
    }
#endif

    // Mask most, to prevent threads (esp. dpdk helper threads)
    // from servicing a signal.  Individual reactors will unmask signals
    // as they become prepared to handle them.
    //
    // We leave some signals unmasked since we don't handle them ourself.
    sigset_t sigs;
    sigfillset(&sigs);
    for (auto sig : {SIGHUP, SIGQUIT, SIGILL, SIGABRT, SIGFPE, SIGSEGV,
            SIGALRM, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU}) {
        sigdelset(&sigs, sig);
    }
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    install_oneshot_signal_handler<SIGSEGV, sigsegv_action>();
    install_oneshot_signal_handler<SIGABRT, sigabrt_action>();

#ifdef SEASTAR_HAVE_DPDK
    _using_dpdk = configuration.count("dpdk-pmd");
#endif
    auto thread_affinity = configuration["thread-affinity"].as<bool>();
    if (configuration.count("overprovisioned")
           && configuration["thread-affinity"].defaulted()) {
        thread_affinity = false;
    }
    if (!thread_affinity && _using_dpdk) {
        fmt::print("warning: --thread-affinity 0 ignored in dpdk mode\n");
    }
    auto mbind = configuration["mbind"].as<bool>();
    if (!thread_affinity) {
        mbind = false;
    }

    smp::count = 1;
    smp::_tmain = std::this_thread::get_id();
    auto nr_cpus = resource::nr_processing_units();
    resource::cpuset cpu_set;
    std::copy(boost::counting_iterator<unsigned>(0), boost::counting_iterator<unsigned>(nr_cpus),
            std::inserter(cpu_set, cpu_set.end()));
    if (configuration.count("cpuset")) {
        cpu_set = configuration["cpuset"].as<cpuset_bpo_wrapper>().value;
    }
    if (configuration.count("smp")) {
        nr_cpus = configuration["smp"].as<unsigned>();
    } else {
        nr_cpus = cpu_set.size();
    }
    smp::count = nr_cpus;
    _reactors.resize(nr_cpus);
    resource::configuration rc;
    if (configuration.count("memory")) {
        rc.total_memory = parse_memory_size(configuration["memory"].as<std::string>());
#ifdef SEASTAR_HAVE_DPDK
        if (configuration.count("hugepages") &&
            !configuration["network-stack"].as<std::string>().compare("native") &&
            _using_dpdk) {
            size_t dpdk_memory = dpdk::eal::mem_size(smp::count);

            if (dpdk_memory >= rc.total_memory) {
                std::cerr<<"Can't run with the given amount of memory: ";
                std::cerr<<configuration["memory"].as<std::string>();
                std::cerr<<". Consider giving more."<<std::endl;
                exit(1);
            }

            //
            // Subtract the memory we are about to give to DPDK from the total
            // amount of memory we are allowed to use.
            //
            rc.total_memory.value() -= dpdk_memory;
        }
#endif
    }
    if (configuration.count("reserve-memory")) {
        rc.reserve_memory = parse_memory_size(configuration["reserve-memory"].as<std::string>());
    }
    compat::optional<std::string> hugepages_path;
    if (configuration.count("hugepages")) {
        hugepages_path = configuration["hugepages"].as<std::string>();
    }
    auto mlock = false;
    if (configuration.count("lock-memory")) {
        mlock = configuration["lock-memory"].as<bool>();
    }
    if (mlock) {
        auto r = mlockall(MCL_CURRENT | MCL_FUTURE);
        if (r) {
            // Don't hard fail for now, it's hard to get the configuration right
            fmt::print("warning: failed to mlockall: {}\n", strerror(errno));
        }
    }

    rc.cpus = smp::count;
    rc.cpu_set = std::move(cpu_set);

    disk_config_params disk_config;
    disk_config.parse_config(configuration);
    for (auto& id : disk_config.device_ids()) {
        rc.num_io_queues.emplace(id, disk_config.num_io_queues(id));
    }

    auto resources = resource::allocate(rc);
    std::vector<resource::cpu> allocations = std::move(resources.cpus);
    if (thread_affinity) {
        smp::pin(allocations[0].cpu_id);
    }
    memory::configure(allocations[0].mem, mbind, hugepages_path);

    if (configuration.count("abort-on-seastar-bad-alloc")) {
        memory::enable_abort_on_allocation_failure();
    }

    bool heapprof_enabled = configuration.count("heapprof");
    memory::set_heap_profiling_enabled(heapprof_enabled);

#ifdef SEASTAR_HAVE_DPDK
    if (smp::_using_dpdk) {
        dpdk::eal::cpuset cpus;
        for (auto&& a : allocations) {
            cpus[a.cpu_id] = true;
        }
        dpdk::eal::init(cpus, configuration);
    }
#endif

    // Better to put it into the smp class, but at smp construction time
    // correct smp::count is not known.
    static boost::barrier reactors_registered(smp::count);
    static boost::barrier smp_queues_constructed(smp::count);
    static boost::barrier inited(smp::count);

    auto ioq_topology = std::move(resources.ioq_topology);

    std::unordered_map<dev_t, std::vector<io_queue*>> all_io_queues;
    io_queue::fill_shares_array();

    for (auto& id : disk_config.device_ids()) {
        auto io_info = ioq_topology.at(id);
        all_io_queues.emplace(id, io_info.coordinators.size());
    }

    auto alloc_io_queue = [&ioq_topology, &all_io_queues, &disk_config] (unsigned shard, dev_t id) {
        auto io_info = ioq_topology.at(id);
        auto cid = io_info.shard_to_coordinator[shard];
        auto vec_idx = io_info.coordinator_to_idx[cid];
        assert(io_info.coordinator_to_idx_valid[cid]);
        if (shard == cid) {
            struct io_queue::config cfg = disk_config.generate_config(id);
            cfg.coordinator = cid;
            cfg.io_topology = io_info.shard_to_coordinator;
            assert(vec_idx < all_io_queues[id].size());
            assert(!all_io_queues[id][vec_idx]);
            all_io_queues[id][vec_idx] = new io_queue(std::move(cfg));
        }
    };

    auto assign_io_queue = [&ioq_topology, &all_io_queues, &disk_config] (shard_id shard_id, dev_t dev_id) {
        auto io_info = ioq_topology.at(dev_id);
        auto cid = io_info.shard_to_coordinator[shard_id];
        auto queue_idx = io_info.coordinator_to_idx[cid];
        if (all_io_queues[dev_id][queue_idx]->coordinator() == shard_id) {
            engine().my_io_queues.emplace_back(all_io_queues[dev_id][queue_idx]);
        }
        engine()._io_queues.emplace(dev_id, all_io_queues[dev_id][queue_idx]);
    };

    _all_event_loops_done.emplace(smp::count);

    auto backend_selector = configuration["reactor-backend"].as<reactor_backend_selector>();

    unsigned i;
    for (i = 1; i < smp::count; i++) {
        auto allocation = allocations[i];
        create_thread([configuration, &disk_config, hugepages_path, i, allocation, assign_io_queue, alloc_io_queue, thread_affinity, heapprof_enabled, mbind, backend_selector] {
            auto thread_name = seastar::format("reactor-{}", i);
            pthread_setname_np(pthread_self(), thread_name.c_str());
            if (thread_affinity) {
                smp::pin(allocation.cpu_id);
            }
            memory::configure(allocation.mem, mbind, hugepages_path);
            memory::set_heap_profiling_enabled(heapprof_enabled);
            sigset_t mask;
            sigfillset(&mask);
            for (auto sig : { SIGSEGV }) {
                sigdelset(&mask, sig);
            }
            auto r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
            throw_pthread_error(r);
            allocate_reactor(i, backend_selector);
            _reactors[i] = &engine();
            for (auto& dev_id : disk_config.device_ids()) {
                alloc_io_queue(i, dev_id);
            }
            reactors_registered.wait();
            smp_queues_constructed.wait();
            start_all_queues();
            for (auto& dev_id : disk_config.device_ids()) {
                assign_io_queue(i, dev_id);
            }
            inited.wait();
            engine().configure(configuration);
            engine().run();
        });
    }

    allocate_reactor(0, backend_selector);
    _reactors[0] = &engine();
    for (auto& dev_id : disk_config.device_ids()) {
        alloc_io_queue(0, dev_id);
    }

#ifdef SEASTAR_HAVE_DPDK
    if (_using_dpdk) {
        auto it = _thread_loops.begin();
        RTE_LCORE_FOREACH_SLAVE(i) {
            rte_eal_remote_launch(dpdk_thread_adaptor, static_cast<void*>(&*(it++)), i);
        }
    }
#endif

    reactors_registered.wait();
    smp::_qs = decltype(smp::_qs){new smp_message_queue* [smp::count], qs_deleter{}};
    for(unsigned i = 0; i < smp::count; i++) {
        smp::_qs[i] = reinterpret_cast<smp_message_queue*>(operator new[] (sizeof(smp_message_queue) * smp::count));
        for (unsigned j = 0; j < smp::count; ++j) {
            new (&smp::_qs[i][j]) smp_message_queue(_reactors[j], _reactors[i]);
        }
    }
    alien::smp::_qs = alien::smp::create_qs(_reactors);
    smp_queues_constructed.wait();
    start_all_queues();
    for (auto& dev_id : disk_config.device_ids()) {
        assign_io_queue(0, dev_id);
    }
    inited.wait();

    engine().configure(configuration);
    // The raw `new` is necessary because of the private constructor of `lowres_clock_impl`.
    engine()._lowres_clock_impl = std::unique_ptr<lowres_clock_impl>(new lowres_clock_impl);
}

bool smp::poll_queues() {
    size_t got = 0;
    for (unsigned i = 0; i < count; i++) {
        if (engine().cpu_id() != i) {
            auto& rxq = _qs[engine().cpu_id()][i];
            rxq.flush_response_batch();
            got += rxq.has_unflushed_responses();
            got += rxq.process_incoming();
            auto& txq = _qs[i][engine()._id];
            txq.flush_request_batch();
            got += txq.process_completions();
        }
    }
    return got != 0;
}

bool smp::pure_poll_queues() {
    for (unsigned i = 0; i < count; i++) {
        if (engine().cpu_id() != i) {
            auto& rxq = _qs[engine().cpu_id()][i];
            rxq.flush_response_batch();
            auto& txq = _qs[i][engine()._id];
            txq.flush_request_batch();
            if (rxq.pure_poll_rx() || txq.pure_poll_tx() || rxq.has_unflushed_responses()) {
                return true;
            }
        }
    }
    return false;
}

internal::preemption_monitor bootstrap_preemption_monitor{};
__thread const internal::preemption_monitor* g_need_preempt = &bootstrap_preemption_monitor;

__thread reactor* local_engine;

#ifdef HAVE_OSV
reactor_backend_osv::reactor_backend_osv() {
}

bool
reactor_backend_osv::wait_and_process() {
    _poller.process();
    // osv::poller::process runs pollable's callbacks, but does not currently
    // have a timer expiration callback - instead if gives us an expired()
    // function we need to check:
    if (_poller.expired()) {
        _timer_promise.set_value();
        _timer_promise = promise<>();
    }
    return true;
}

future<>
reactor_backend_osv::readable(pollable_fd_state& fd) {
    std::cout << "reactor_backend_osv does not support file descriptors - readable() shouldn't have been called!\n";
    abort();
}

future<>
reactor_backend_osv::writeable(pollable_fd_state& fd) {
    std::cout << "reactor_backend_osv does not support file descriptors - writeable() shouldn't have been called!\n";
    abort();
}

void
reactor_backend_osv::forget(pollable_fd_state& fd) {
    std::cout << "reactor_backend_osv does not support file descriptors - forget() shouldn't have been called!\n";
    abort();
}

void
reactor_backend_osv::enable_timer(steady_clock_type::time_point when) {
    _poller.set_timer(when);
}

#endif

void report_exception(compat::string_view message, std::exception_ptr eptr) noexcept {
    seastar_logger.error("{}: {}", message, eptr);
}

/**
 * engine_exit() exits the reactor. It should be given a pointer to the
 * exception which prompted this exit - or a null pointer if the exit
 * request was not caused by any exception.
 */
void engine_exit(std::exception_ptr eptr) {
    if (!eptr) {
        engine().exit(0);
        return;
    }
    report_exception("Exiting on unhandled exception", eptr);
    engine().exit(1);
}

void report_failed_future(std::exception_ptr eptr) {
    seastar_logger.warn("Exceptional future ignored: {}, backtrace: {}", eptr, current_backtrace());
}

future<> check_direct_io_support(sstring path) {
    struct w {
        sstring path;
        open_flags flags;
        std::function<future<>()> cleanup;

        static w parse(sstring path, compat::optional<directory_entry_type> type) {
            if (!type) {
                throw std::invalid_argument(format("Could not open file at {}. Make sure it exists", path));
            }

            if (type == directory_entry_type::directory) {
                auto fpath = path + "/.o_direct_test";
                return w{fpath, open_flags::wo | open_flags::create | open_flags::truncate, [fpath] { return remove_file(fpath); }};
            } else if ((type == directory_entry_type::regular) || (type == directory_entry_type::link)) {
                return w{path, open_flags::ro, [] { return make_ready_future<>(); }};
            } else {
                throw std::invalid_argument(format("{} neither a directory nor file. Can't be opened with O_DIRECT", path));
            }
        };
    };

    return engine().file_type(path).then([path] (auto type) {
        auto w = w::parse(path, type);
        return open_file_dma(w.path, w.flags).then_wrapped([path = w.path, cleanup = std::move(w.cleanup)] (future<file> f) {
            try {
                f.get0();
                return cleanup();
            } catch (std::system_error& e) {
                if (e.code() == std::error_code(EINVAL, std::system_category())) {
                    report_exception(format("Could not open file at {}. Does your filesystem support O_DIRECT?", path), std::current_exception());
                }
                throw;
            }
        });
    });
}

future<file> open_file_dma(sstring name, open_flags flags) {
    return engine().open_file_dma(std::move(name), flags, file_open_options());
}

future<file> open_file_dma(sstring name, open_flags flags, file_open_options options) {
    return engine().open_file_dma(std::move(name), flags, options);
}

future<file> open_directory(sstring name) {
    return engine().open_directory(std::move(name));
}

future<> make_directory(sstring name) {
    return engine().make_directory(std::move(name));
}

future<> touch_directory(sstring name) {
    return engine().touch_directory(std::move(name));
}

future<> sync_directory(sstring name) {
    return open_directory(std::move(name)).then([] (file f) {
        return do_with(std::move(f), [] (file& f) {
            return f.flush().then([&f] () mutable {
                return f.close();
            });
        });
    });
}

future<> do_recursive_touch_directory(sstring base, sstring name) {
    static const sstring::value_type separator = '/';

    if (name.empty()) {
        return make_ready_future<>();
    }

    size_t pos = std::min(name.find(separator), name.size() - 1);
    base += name.substr(0 , pos + 1);
    name = name.substr(pos + 1);
    return touch_directory(base).then([base, name] {
        return do_recursive_touch_directory(base, name);
    }).then([base] {
        // We will now flush the directory that holds the entry we potentially
        // created. Technically speaking, we only need to touch when we did
        // create. But flushing the unchanged ones should be cheap enough - and
        // it simplifies the code considerably.
        if (base.empty()) {
            return make_ready_future<>();
        }

        return sync_directory(base);
    });
}
/// \endcond

future<> recursive_touch_directory(sstring name) {
    // If the name is empty,  it will be of the type a/b/c, which should be interpreted as
    // a relative path. This means we have to flush our current directory
    sstring base = "";
    if (name[0] != '/' || name[0] == '.') {
        base = "./";
    }
    return do_recursive_touch_directory(base, name);
}

future<> remove_file(sstring pathname) {
    return engine().remove_file(std::move(pathname));
}

future<> rename_file(sstring old_pathname, sstring new_pathname) {
    return engine().rename_file(std::move(old_pathname), std::move(new_pathname));
}

future<fs_type> file_system_at(sstring name) {
    return engine().file_system_at(name);
}

future<uint64_t> fs_avail(sstring name) {
    return engine().statvfs(name).then([] (struct statvfs st) {
        return make_ready_future<uint64_t>(st.f_bavail * st.f_frsize);
    });
}

future<uint64_t> fs_free(sstring name) {
    return engine().statvfs(name).then([] (struct statvfs st) {
        return make_ready_future<uint64_t>(st.f_bfree * st.f_frsize);
    });
}

future<uint64_t> file_size(sstring name) {
    return engine().file_size(name);
}

future<bool> file_accessible(sstring name, access_flags flags) {
    return engine().file_accessible(name, flags);
}

future<bool> file_exists(sstring name) {
    return engine().file_exists(name);
}

future<> link_file(sstring oldpath, sstring newpath) {
    return engine().link_file(std::move(oldpath), std::move(newpath));
}

server_socket listen(socket_address sa) {
    return engine().listen(sa);
}

server_socket listen(socket_address sa, listen_options opts) {
    return engine().listen(sa, opts);
}

future<connected_socket> connect(socket_address sa) {
    return engine().connect(sa);
}

future<connected_socket> connect(socket_address sa, socket_address local, transport proto = transport::TCP) {
    return engine().connect(sa, local, proto);
}

void reactor::add_high_priority_task(std::unique_ptr<task>&& t) {
    add_urgent_task(std::move(t));
    // break .then() chains
    request_preemption();
}

static
bool
virtualized() {
    return fs::exists("/sys/hypervisor/type");
}

std::chrono::nanoseconds
reactor::calculate_poll_time() {
    // In a non-virtualized environment, select a poll time
    // that is competitive with halt/unhalt.
    //
    // In a virutalized environment, IPIs are slow and dominate
    // sleep/wake (mprotect/tgkill), so increase poll time to reduce
    // so we don't sleep in a request/reply workload
    return virtualized() ? 2000us : 200us;
}

future<> later() {
    promise<> p;
    auto f = p.get_future();
    engine().force_poll();
    schedule(make_task(default_scheduling_group(), [p = std::move(p)] () mutable {
        p.set_value();
    }));
    return f;
}

void add_to_flush_poller(output_stream<char>* os) {
    engine()._flush_batching.emplace_back(os);
}

reactor::sched_clock::duration reactor::total_idle_time() {
    return _total_idle;
}

reactor::sched_clock::duration reactor::total_busy_time() {
    return sched_clock::now() - _start_time - _total_idle;
}

std::chrono::nanoseconds reactor::total_steal_time() {
    // Steal time: this mimics the concept some Hypervisors have about Steal time.
    // That is the time in which a VM has something to run, but is not running because some other
    // process (another VM or the hypervisor itself) is in control.
    //
    // For us, we notice that during the time in which we were not sleeping (either running or busy
    // polling while idle), we should be accumulating thread runtime. If we are not, that's because
    // someone stole it from us.
    //
    // Because this is totally in userspace we can miss some events. For instance, if the seastar
    // process is ready to run but the kernel hasn't scheduled us yet, that would be technically
    // steal time but we have no ways to account it.
    //
    // But what we have here should be good enough and at least has a well defined meaning.
    return std::chrono::duration_cast<std::chrono::nanoseconds>(sched_clock::now() - _start_time - _total_sleep) -
           std::chrono::duration_cast<std::chrono::nanoseconds>(thread_cputime_clock::now().time_since_epoch());
}

static std::atomic<unsigned long> s_used_scheduling_group_ids_bitmap{3}; // 0=main, 1=atexit

static
unsigned
allocate_scheduling_group_id() {
    static_assert(max_scheduling_groups() <= std::numeric_limits<unsigned long>::digits);
    auto b = s_used_scheduling_group_ids_bitmap.load(std::memory_order_relaxed);
    auto nb = b;
    unsigned i = 0;
    do {
        if (__builtin_popcountl(b) == max_scheduling_groups()) {
            throw std::runtime_error("Scheduling group limit exceeded");
        }
        i = count_trailing_zeros(~b);
        nb = b | (1ul << i);
    } while (!s_used_scheduling_group_ids_bitmap.compare_exchange_weak(b, nb, std::memory_order_relaxed));
    return i;
}

static
void
deallocate_scheduling_group_id(unsigned id) {
    s_used_scheduling_group_ids_bitmap.fetch_and(~(1ul << id), std::memory_order_relaxed);
}

void
reactor::init_scheduling_group(seastar::scheduling_group sg, sstring name, float shares) {
    _task_queues.resize(std::max<size_t>(_task_queues.size(), sg._id + 1));
    _task_queues[sg._id] = std::make_unique<task_queue>(sg._id, name, shares);
}

void
reactor::destroy_scheduling_group(scheduling_group sg) {
    _task_queues[sg._id].reset();
}

const sstring&
scheduling_group::name() const {
    return engine()._task_queues[_id]->_name;
}

void
scheduling_group::set_shares(float shares) {
    engine()._task_queues[_id]->set_shares(shares);
}

future<scheduling_group>
create_scheduling_group(sstring name, float shares) {
    auto id = allocate_scheduling_group_id();
    assert(id < max_scheduling_groups());
    auto sg = scheduling_group(id);
    return smp::invoke_on_all([sg, name, shares] {
        engine().init_scheduling_group(sg, name, shares);
    }).then([sg] {
        return make_ready_future<scheduling_group>(sg);
    });
}

future<>
destroy_scheduling_group(scheduling_group sg) {
    if (sg == default_scheduling_group()) {
        throw_with_backtrace<std::runtime_error>("Attempt to destroy the default scheduling group");
    }
    if (sg == current_scheduling_group()) {
        throw_with_backtrace<std::runtime_error>("Attempt to destroy the current scheduling group");
    }
    return smp::invoke_on_all([sg] {
        engine().destroy_scheduling_group(sg);
    }).then([sg] {
        deallocate_scheduling_group_id(sg._id);
    });
}


namespace internal {

inline
std::chrono::steady_clock::duration
timeval_to_duration(::timeval tv) {
    return std::chrono::seconds(tv.tv_sec) + std::chrono::microseconds(tv.tv_usec);
}

class reactor_stall_sampler : public reactor::pollfn {
    std::chrono::steady_clock::time_point _run_start;
    ::rusage _run_start_rusage;
    uint64_t _kernel_stalls = 0;
    std::chrono::steady_clock::duration _nonsleep_cpu_time = {};
    std::chrono::steady_clock::duration _nonsleep_wall_time = {};
private:
    static ::rusage get_rusage() {
        struct ::rusage ru;
        ::getrusage(RUSAGE_THREAD, &ru);
        return ru;
    }
    static std::chrono::steady_clock::duration cpu_time(const ::rusage& ru) {
        return timeval_to_duration(ru.ru_stime) + timeval_to_duration(ru.ru_utime);
    }
    void mark_run_start() {
        _run_start = std::chrono::steady_clock::now();
        _run_start_rusage = get_rusage();
    }
    void mark_run_end() {
        auto start_nvcsw = _run_start_rusage.ru_nvcsw;
        auto start_cpu_time = cpu_time(_run_start_rusage);
        auto start_time = _run_start;
        _run_start = std::chrono::steady_clock::now();
        _run_start_rusage = get_rusage();
        _kernel_stalls += _run_start_rusage.ru_nvcsw - start_nvcsw;
        _nonsleep_cpu_time += cpu_time(_run_start_rusage) - start_cpu_time;
        _nonsleep_wall_time += _run_start - start_time;
    }
public:
    reactor_stall_sampler() { mark_run_start(); }
    virtual bool poll() override { return false; }
    virtual bool pure_poll() override { return false; }
    virtual bool try_enter_interrupt_mode() override {
        // try_enter_interrupt_mode marks the end of a reactor run that should be context-switch free
        mark_run_end();
        return true;
    }
    virtual void exit_interrupt_mode() override {
        // start a reactor run that should be context switch free
        mark_run_start();
    }
    stall_report report() const {
        stall_report r;
        // mark_run_end() with an immediate mark_run_start() is logically a no-op,
        // but each one of them has an effect, so they can't be marked const
        const_cast<reactor_stall_sampler*>(this)->mark_run_end();
        r.kernel_stalls = _kernel_stalls;
        r.run_wall_time = _nonsleep_wall_time;
        r.stall_time = _nonsleep_wall_time - _nonsleep_cpu_time;
        const_cast<reactor_stall_sampler*>(this)->mark_run_start();
        return r;
    }
};

future<stall_report>
report_reactor_stalls(noncopyable_function<future<> ()> uut) {
    auto reporter = std::make_unique<reactor_stall_sampler>();
    auto p_reporter = reporter.get();
    auto poller = reactor::poller(std::move(reporter));
    return uut().then([poller = std::move(poller), p_reporter] () mutable {
        return p_reporter->report();
    });
}

std::ostream& operator<<(std::ostream& os, const stall_report& sr) {
    auto to_ms = [] (std::chrono::steady_clock::duration d) -> float {
        return std::chrono::duration<float>(d) / 1ms;
    };
    return os << format("{} stalls, {} ms stall time, {} ms run time", sr.kernel_stalls, to_ms(sr.stall_time), to_ms(sr.run_wall_time));
}

}

}
