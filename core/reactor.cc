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

#include <sys/syscall.h>
#include <sys/vfs.h>
#include "task.hh"
#include "reactor.hh"
#include "memory.hh"
#include "core/posix.hh"
#include "net/packet.hh"
#include "net/stack.hh"
#include "net/posix-stack.hh"
#include "resource.hh"
#include "print.hh"
#include "scollectd-impl.hh"
#include "util/conversions.hh"
#include "core/future-util.hh"
#include "thread.hh"
#include "systemwide_memory_barrier.hh"
#include "report_exception.hh"
#include <cassert>
#include <unistd.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <boost/filesystem.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/range/numeric.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <atomic>
#include <dirent.h>
#include <linux/types.h> // for xfs, below
#include <sys/ioctl.h>
#include <xfs/linux.h>
#define min min    /* prevent xfs.h from defining min() as a macro */
#include <xfs/xfs.h>
#undef min
#ifdef HAVE_DPDK
#include <core/dpdk_rte.hh>
#include <rte_lcore.h>
#include <rte_launch.h>
#endif
#include "prefetch.hh"
#include <exception>
#include <regex>
#ifdef __GNUC__
#include <iostream>
#include <system_error>
#include <cxxabi.h>
#endif

#include <linux/falloc.h>
#include <linux/magic.h>

#ifdef HAVE_OSV
#include <osv/newpoll.hh>
#endif

#include <xmmintrin.h>

using namespace std::chrono_literals;

using namespace net;

std::atomic<lowres_clock::rep> lowres_clock::_now;
constexpr std::chrono::milliseconds lowres_clock::_granularity;
static thread_local uint64_t logging_failures = 0;

timespec to_timespec(steady_clock_type::time_point t) {
    using ns = std::chrono::nanoseconds;
    auto n = std::chrono::duration_cast<ns>(t.time_since_epoch()).count();
    return { n / 1'000'000'000, n % 1'000'000'000 };
}

lowres_clock::lowres_clock() {
    update();
    _timer.set_callback([this] { update(); });
    _timer.arm_periodic(_granularity);
}

void lowres_clock::update() {
    using namespace std::chrono;
    auto now = steady_clock_type::now();
    auto ticks = duration_cast<milliseconds>(now.time_since_epoch()).count();
    _now.store(ticks, std::memory_order_relaxed);
}

template <typename T>
struct syscall_result {
    T result;
    int error;
    void throw_if_error() {
        if (long(result) == -1) {
            throw std::system_error(error, std::system_category());
        }
    }
};

// Wrapper for a system call result containing the return value,
// an output parameter that was returned from the syscall, and errno.
template <typename Extra>
struct syscall_result_extra {
    int result;
    Extra extra;
    int error;
    void throw_if_error() {
        if (result == -1) {
            throw std::system_error(error, std::system_category());
        }
    }
};

template <typename T>
syscall_result<T>
wrap_syscall(T result) {
    syscall_result<T> sr;
    sr.result = result;
    sr.error = errno;
    return sr;
}

template <typename Extra>
syscall_result_extra<Extra>
wrap_syscall(int result, const Extra& extra) {
    return {result, extra, errno};
}

reactor_backend_epoll::reactor_backend_epoll()
    : _epollfd(file_desc::epoll_create(EPOLL_CLOEXEC)) {
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
    struct sigaction sa;
    sa.sa_sigaction = action;
    sa.sa_mask = make_empty_sigset_mask();
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    auto r = ::sigaction(signo, &sa, nullptr);
    throw_system_error_on(r == -1);
    auto mask = make_sigset_mask(signo);
    r = ::pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
    throw_system_error_on(r == -1);
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

void reactor::signals::action(int signo, siginfo_t* siginfo, void* ignore) {
    engine()._signals._pending_signals.fetch_or(1ull << signo, std::memory_order_relaxed);
}

inline int alarm_signal() {
    // We don't want to use SIGALRM, because the boost unit test library
    // also plays with it.
    return SIGRTMIN;
}

inline int task_quota_signal() {
    return SIGRTMIN + 1;
}

reactor::reactor()
    : _backend()
#ifdef HAVE_OSV
    , _timer_thread(
        [&] { timer_thread_func(); }, sched::thread::attr().stack(4096).name("timer_thread").pin(sched::cpu::current()))
    , _engine_thread(sched::thread::current())
#endif
    , _cpu_started(0)
    , _io_context(0)
    , _io_context_available(max_aio)
    , _reuseport(posix_reuseport_detect()) {

    seastar::thread_impl::init();
    auto r = ::io_setup(max_aio, &_io_context);
    assert(r >= 0);
#ifdef HAVE_OSV
    _timer_thread.start();
#else
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, alarm_signal());
    r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
    assert(r == 0);
    struct sigevent sev;
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev._sigev_un._tid = syscall(SYS_gettid);
    sev.sigev_signo = alarm_signal();
    r = timer_create(CLOCK_MONOTONIC, &sev, &_steady_clock_timer);
    assert(r >= 0);
    sev.sigev_signo = task_quota_signal();
    r = timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &_task_quota_timer);
    assert(r >= 0);
    sigemptyset(&mask);
    sigaddset(&mask, task_quota_signal());
    r = ::pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
    assert(r == 0);
#endif
    memory::set_reclaim_hook([this] (std::function<void ()> reclaim_fn) {
        add_high_priority_task(make_task([fn = std::move(reclaim_fn)] {
            fn();
        }));
    });
}

reactor::~reactor() {
    timer_delete(_task_quota_timer);
    timer_delete(_steady_clock_timer);
    auto eraser = [](auto& list) {
        while (!list.empty()) {
            auto& timer = *list.begin();
            timer.cancel();
        }
    };
    eraser(_expired_timers);
    eraser(_expired_lowres_timers);
}

void
reactor::clear_task_quota(int) {
    future_avail_count = max_inlined_continuations - 1;
    local_engine->_task_quota_finished = true;
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
            } catch (std::exception& e) {
                std::cerr << "Timer callback failed: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Timer callback failed for unknown reason" << std::endl;
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
                    _pending_tasks.push_front(make_task([this] {
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
    static void register_stack(sstring name,
            boost::program_options::options_description opts,
            std::function<future<std::unique_ptr<network_stack>> (options opts)> create,
            bool make_default = false);
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
    _task_quota = vm["task-quota-ms"].as<double>() * 1ms;
    if (vm.count("poll-mode")) {
        _max_poll_time = std::chrono::nanoseconds::max();
    }
    set_strict_dma(!vm.count("relaxed-dma"));
}

future<> reactor_backend_epoll::get_epoll_future(pollable_fd_state& pfd,
        promise<> pollable_fd_state::*pr, int event) {
    if (pfd.events_known & event) {
        pfd.events_known &= ~event;
        return make_ready_future();
    }
    pfd.events_requested |= event;
    if (!(pfd.events_epoll & event)) {
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

void reactor_backend_epoll::abort_fd(pollable_fd_state& pfd, std::exception_ptr ex,
                                     promise<> pollable_fd_state::* pr, int event) {
    if (pfd.events_epoll & event) {
        pfd.events_epoll &= ~event;
        auto ctl = pfd.events_epoll ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        ::epoll_event eevt;
        eevt.events = pfd.events_epoll;
        eevt.data.ptr = &pfd;
        int r = ::epoll_ctl(_epollfd.get(), ctl, pfd.fd.get(), &eevt);
        assert(r == 0);
    }
    if (pfd.events_requested & event) {
        pfd.events_requested &= ~event;
        (pfd.*pr).set_exception(std::move(ex));
    }
    pfd.events_known &= ~event;
}

future<> reactor_backend_epoll::readable(pollable_fd_state& fd) {
    return get_epoll_future(fd, &pollable_fd_state::pollin, EPOLLIN);
}

future<> reactor_backend_epoll::writeable(pollable_fd_state& fd) {
    return get_epoll_future(fd, &pollable_fd_state::pollout, EPOLLOUT);
}

void reactor_backend_epoll::abort_reader(pollable_fd_state& fd, std::exception_ptr ex) {
    abort_fd(fd, std::move(ex), &pollable_fd_state::pollin, EPOLLIN);
}

void reactor_backend_epoll::abort_writer(pollable_fd_state& fd, std::exception_ptr ex) {
    abort_fd(fd, std::move(ex), &pollable_fd_state::pollout, EPOLLOUT);
}

void reactor_backend_epoll::forget(pollable_fd_state& fd) {
    if (fd.events_epoll) {
        ::epoll_ctl(_epollfd.get(), EPOLL_CTL_DEL, fd.fd.get(), nullptr);
    }
}

future<> reactor_backend_epoll::notified(reactor_notifier *n) {
    // Currently reactor_backend_epoll doesn't need to support notifiers,
    // because we add to it file descriptors instead. But this can be fixed
    // later.
    std::cout << "reactor_backend_epoll does not yet support notifiers!\n";
    abort();
}


pollable_fd
reactor::posix_listen(socket_address sa, listen_options opts) {
    file_desc fd = file_desc::socket(sa.u.sa.sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
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

future<pollable_fd>
reactor::posix_connect(socket_address sa, socket_address local) {
    file_desc fd = file_desc::socket(sa.u.sa.sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    fd.bind(local.u.sa, sizeof(sa.u.sas));
    fd.connect(sa.u.sa, sizeof(sa.u.sas));
    auto pfd = pollable_fd(std::move(fd));
    auto f = pfd.writeable();
    return f.then([pfd = std::move(pfd)] () mutable {
        auto err = pfd.get_file_desc().getsockopt<int>(SOL_SOCKET, SO_ERROR);
        if (err != 0) {
            throw std::system_error(err, std::system_category());
        }
        return make_ready_future<pollable_fd>(std::move(pfd));
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
reactor::connect(socket_address sa, socket_address local) {
    return _network_stack->connect(sa, local);
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

template <typename Func>
future<io_event>
reactor::submit_io(Func prepare_io) {
    return _io_context_available.wait(1).then([this, prepare_io = std::move(prepare_io)] () mutable {
        auto pr = std::make_unique<promise<io_event>>();
        iocb io;
        prepare_io(io);
        io.data = pr.get();
        _pending_aio.push_back(io);
        if ((_io_queue->queued_requests() > 0) ||
            (_pending_aio.size() >= std::min(max_aio / 4, _io_queue->_capacity / 2))) {
            flush_pending_aio();
        }
        return pr.release()->get_future();
    });
}

bool
reactor::flush_pending_aio() {
    bool did_work = false;
    while (!_pending_aio.empty()) {
        auto nr = _pending_aio.size();
        struct iocb* iocbs[max_aio];
        for (size_t i = 0; i < nr; ++i) {
            iocbs[i] = &_pending_aio[i];
        }
        auto r = ::io_submit(_io_context, nr, iocbs);
        size_t nr_consumed;
        if (r < 0) {
            auto ec = -r;
            switch (ec) {
                case EAGAIN:
                    return did_work;
                case EBADF: {
                    auto pr = reinterpret_cast<promise<io_event>*>(iocbs[0]->data);
                    try {
                        throw_kernel_error(r);
                    } catch (...) {
                        pr->set_exception(std::current_exception());
                    }
                    delete pr;
                    _io_context_available.signal(1);
                    nr_consumed = 1;
                    break;
                }
                default:
                    throw_kernel_error(r);
                    abort();
            }
        } else {
            nr_consumed = size_t(r);
        }

        did_work = true;
        if (nr_consumed == nr) {
            _pending_aio.clear();
        } else {
            _pending_aio.erase(_pending_aio.begin(), _pending_aio.begin() + r);
        }
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
reactor::submit_io_read(const io_priority_class& pc, size_t len, Func prepare_io) {
    ++_aio_reads;
    _aio_read_bytes += len;
    return io_queue::queue_request(_io_coordinator, pc, len, std::move(prepare_io));
}

template <typename Func>
future<io_event>
reactor::submit_io_write(const io_priority_class& pc, size_t len, Func prepare_io) {
    ++_aio_writes;
    _aio_write_bytes += len;
    return io_queue::queue_request(_io_coordinator, pc, len, std::move(prepare_io));
}

bool reactor::process_io()
{
    io_event ev[max_aio];
    struct timespec timeout = {0, 0};
    auto n = ::io_getevents(_io_context, 1, max_aio, ev, &timeout);
    assert(n >= 0);
    for (size_t i = 0; i < size_t(n); ++i) {
        auto pr = reinterpret_cast<promise<io_event>*>(ev[i].data);
        pr->set_value(ev[i]);
        delete pr;
    }
    _io_context_available.signal(n);
    return n;
}

io_queue::io_queue(shard_id coordinator, size_t capacity, std::vector<shard_id> topology)
        : _coordinator(coordinator)
        , _capacity(capacity)
        , _io_topology(std::move(topology))
        , _priority_classes()
        , _fq(capacity) {
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
    uint32_t unused = 0;
    for (unsigned i = 0; i < _max_classes; ++i) {
        auto s = _registered_shares[i].compare_exchange_strong(unused, shares, std::memory_order_acq_rel);
        if (s) {
            io_priority_class p;
            _registered_names[i] = name;
            p.val = i;
            return std::move(p);
        };
    }
    throw std::runtime_error("No more room for new I/O priority classes");
}

io_queue::priority_class_data::priority_class_data(sstring name, priority_class_ptr ptr)
    : ptr(ptr)
    , bytes(0)
    , ops(0)
    , nr_queued(0)
    , queue_time(1s)
    , collectd_reg(scollectd::registrations({
        scollectd::add_polled_metric(scollectd::type_instance_id("io_queue"
            , scollectd::per_cpu_plugin_instance
            , "derive", name)
            , scollectd::make_typed(scollectd::data_type::DERIVE, bytes)
        ),
        scollectd::add_polled_metric(scollectd::type_instance_id("io_queue"
            , scollectd::per_cpu_plugin_instance
            , "total_operations", name)
            , scollectd::make_typed(scollectd::data_type::DERIVE, ops)
        ),

        // Note: The counter below is not the same as reactor's queued-io-requests
        // queued-io-requests shows us how many requests in total exist in this I/O Queue.
        //
        // This counter lives in the priority class, so it will count only queued requests
        // that belong to that class.
        //
        // In other words: the new counter tells you how busy a class is, and the
        // old counter tells you how busy the system is.
        scollectd::add_polled_metric(scollectd::type_instance_id("io_queue"
            , scollectd::per_cpu_plugin_instance
            , "queue_length", name)
            , scollectd::make_typed(scollectd::data_type::GAUGE, nr_queued)
        ),
        scollectd::add_polled_metric(scollectd::type_instance_id("io_queue"
            , scollectd::per_cpu_plugin_instance
            , "delay", name)
            , scollectd::make_typed(scollectd::data_type::GAUGE, [this] {
                return queue_time.count();
            })
        )
    }))
{
}

io_queue::priority_class_data& io_queue::find_or_create_class(const io_priority_class& pc, shard_id owner) {
    auto it_pclass = _priority_classes.find(pc);
    if (it_pclass == _priority_classes.end()) {
        auto shares = _registered_shares.at(pc).load(std::memory_order_acquire);
        auto name = _registered_names.at(pc);
        // A note on naming:
        //
        // We could just add the owner as the instance id and have something like:
        //  io_queue-<class_owner>-<counter>-<class_name>
        //
        // However, when there are more than one shard per I/O queue, it is very useful
        // to know which shards are being served by the same queue. Therefore, a better name
        // scheme is:
        //
        //  io_queue-<queue_owner>-<counter>-<class_name>-<class_owner>
        //
        // This conveys all the information we need and allows one to easily group all classes from
        // the same I/O queue (by filtering by instance ID)
        auto ret = _priority_classes.emplace(pc, make_lw_shared<priority_class_data>(sprint("%s-%d", name, owner), _fq.register_priority_class(shares)));
        it_pclass = ret.first;
    }
    return *(it_pclass->second);
}

template <typename Func>
future<io_event>
io_queue::queue_request(shard_id coordinator, const io_priority_class& pc, size_t len, Func prepare_io) {
    auto start = std::chrono::steady_clock::now();
    return smp::submit_to(coordinator, [start, &pc, len, prepare_io = std::move(prepare_io), owner = engine().cpu_id()] {
        auto& queue = *(engine()._io_queue);
        unsigned weight = 1 + len/(16 << 10);
        // First time will hit here, and then we create the class. It is important
        // that we create the shared pointer in the same shard it will be used at later.
        auto& pclass = queue.find_or_create_class(pc, owner);
        pclass.bytes += len;
        pclass.ops++;
        pclass.nr_queued++;
        return queue._fq.queue(pclass.ptr, weight, [&pclass, start, prepare_io = std::move(prepare_io)] {
            pclass.nr_queued--;
            pclass.queue_time = std::chrono::duration_cast<std::chrono::duration<double>>(std::chrono::steady_clock::now() - start);
            return engine().submit_io(std::move(prepare_io));
        });
    });
}

file_impl* file_impl::get_file_impl(file& f) {
    return f._file_impl.get();
}

posix_file_impl::posix_file_impl(int fd, file_open_options options)
        : _fd(fd) {
    query_dma_alignment();
}

posix_file_impl::~posix_file_impl() {
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
    return engine().submit_io_write(io_priority_class, len, [fd = _fd, pos, buffer, len] (iocb& io) {
        io_prep_pwrite(&io, fd, const_cast<void*>(buffer), len, pos);
    }).then([] (io_event ev) {
        throw_kernel_error(long(ev.res));
        return make_ready_future<size_t>(size_t(ev.res));
    });
}

future<size_t>
posix_file_impl::write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& io_priority_class) {
    auto len = boost::accumulate(iov | boost::adaptors::transformed(std::mem_fn(&iovec::iov_len)), size_t(0));
    auto iov_ptr = std::make_unique<std::vector<iovec>>(std::move(iov));
    auto size = iov_ptr->size();
    auto data = iov_ptr->data();
    return engine().submit_io_write(io_priority_class, len, [fd = _fd, pos, data, size] (iocb& io) {
        io_prep_pwritev(&io, fd, data, size, pos);
    }).then([iov_ptr = std::move(iov_ptr)] (io_event ev) {
        throw_kernel_error(long(ev.res));
        return make_ready_future<size_t>(size_t(ev.res));
    });
}

future<size_t>
posix_file_impl::read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& io_priority_class) {
    return engine().submit_io_read(io_priority_class, len, [fd = _fd, pos, buffer, len] (iocb& io) {
        io_prep_pread(&io, fd, buffer, len, pos);
    }).then([] (io_event ev) {
        throw_kernel_error(long(ev.res));
        return make_ready_future<size_t>(size_t(ev.res));
    });
}

future<size_t>
posix_file_impl::read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& io_priority_class) {
    auto len = boost::accumulate(iov | boost::adaptors::transformed(std::mem_fn(&iovec::iov_len)), size_t(0));
    auto iov_ptr = std::make_unique<std::vector<iovec>>(std::move(iov));
    auto size = iov_ptr->size();
    auto data = iov_ptr->data();
    return engine().submit_io_read(io_priority_class, len, [fd = _fd, pos, data, size] (iocb& io) {
        io_prep_preadv(&io, fd, data, size, pos);
    }).then([iov_ptr = std::move(iov_ptr)] (io_event ev) {
        throw_kernel_error(long(ev.res));
        return make_ready_future<size_t>(size_t(ev.res));
    });
}

inline
shared_ptr<file_impl>
make_file_impl(int fd, file_open_options options) {
    auto r = ::ioctl(fd, BLKGETSIZE);
    if (r != -1) {
        return make_shared<blockdev_file_impl>(fd, options);
    } else {
        return make_shared<posix_file_impl>(fd, options);
    }
}

file::file(int fd, file_open_options options)
        : _file_impl(make_file_impl(fd, options)) {
}

future<file>
reactor::open_file_dma(sstring name, open_flags flags, file_open_options options) {
    return _thread_pool.submit<syscall_result<int>>([name, flags, options, strict_o_direct = _strict_o_direct] {
        auto open_flags = O_DIRECT | O_CLOEXEC | static_cast<int>(flags);
        int fd = ::open(name.c_str(), open_flags, S_IRWXU);
        if (!strict_o_direct && fd == -1 && errno == EINVAL) {
            // open with O_DIRECT on tmppfs creates the file, then returns an
            // EINVAL; so we must remove O_EXCL as well.
            open_flags &= ~(O_DIRECT | O_EXCL);
            fd = ::open(name.c_str(), open_flags, S_IRWXU);
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
    }).then([options] (syscall_result<int> sr) {
        sr.throw_if_error();
        return make_ready_future<file>(file(sr.result, options));
    });
}

future<>
reactor::remove_file(sstring pathname) {
    return engine()._thread_pool.submit<syscall_result<int>>([this, pathname] {
        return wrap_syscall<int>(::remove(pathname.c_str()));
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
        return make_ready_future<>();
    });
}

future<>
reactor::rename_file(sstring old_pathname, sstring new_pathname) {
    return engine()._thread_pool.submit<syscall_result<int>>([this, old_pathname, new_pathname] {
        return wrap_syscall<int>(::rename(old_pathname.c_str(), new_pathname.c_str()));
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
        return make_ready_future<>();
    });
}

future<>
reactor::link_file(sstring oldpath, sstring newpath) {
    return engine()._thread_pool.submit<syscall_result<int>>([this, oldpath = std::move(oldpath), newpath = std::move(newpath)] {
        return wrap_syscall<int>(::link(oldpath.c_str(), newpath.c_str()));
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
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

future<std::experimental::optional<directory_entry_type>>
reactor::file_type(sstring name) {
    return _thread_pool.submit<syscall_result_extra<struct stat>>([name] {
        struct stat st;
        auto ret = stat(name.c_str(), &st);
        return wrap_syscall(ret, st);
    }).then([] (syscall_result_extra<struct stat> sr) {
        if (long(sr.result) == -1) {
            if (sr.error != ENOENT && sr.error != ENOTDIR) {
                sr.throw_if_error();
            }
            return make_ready_future<std::experimental::optional<directory_entry_type> >
                (std::experimental::optional<directory_entry_type>() );
        }
        return make_ready_future<std::experimental::optional<directory_entry_type> >
            (std::experimental::optional<directory_entry_type>(stat_to_entry_type(sr.extra.st_mode)) );
    });
}

future<uint64_t>
reactor::file_size(sstring pathname) {
    return _thread_pool.submit<syscall_result_extra<struct stat>>([pathname] {
        struct stat st;
        auto ret = stat(pathname.c_str(), &st);
        return wrap_syscall(ret, st);
    }).then([] (syscall_result_extra<struct stat> sr) {
        sr.throw_if_error();
        return make_ready_future<uint64_t>(sr.extra.st_size);
    });
}

future<bool>
reactor::file_exists(sstring pathname) {
    return _thread_pool.submit<syscall_result_extra<struct stat>>([pathname] {
        struct stat st;
        auto ret = stat(pathname.c_str(), &st);
        return wrap_syscall(ret, st);
    }).then([] (syscall_result_extra<struct stat> sr) {
        if (sr.result < 0 && sr.error == ENOENT) {
            return make_ready_future<bool>(false);
        }
        sr.throw_if_error();
        return make_ready_future<bool>(true);
    });
}

future<fs_type>
reactor::file_system_at(sstring pathname) {
    return _thread_pool.submit<syscall_result_extra<struct statfs>>([pathname] {
        struct statfs st;
        auto ret = statfs(pathname.c_str(), &st);
        return wrap_syscall(ret, st);
    }).then([] (syscall_result_extra<struct statfs> sr) {
        static std::unordered_map<long int, fs_type> type_mapper = {
            { 0x58465342, fs_type::xfs },
            { EXT2_SUPER_MAGIC, fs_type::ext2 },
            { EXT3_SUPER_MAGIC, fs_type::ext3 },
            { EXT4_SUPER_MAGIC, fs_type::ext4 },
            { BTRFS_SUPER_MAGIC, fs_type::btrfs },
            { 0x4244, fs_type::hfs },
            { TMPFS_MAGIC, fs_type::tmpfs },
        };
        sr.throw_if_error();

        fs_type ret = fs_type::other;
        if (type_mapper.count(sr.extra.f_type) != 0) {
            ret = type_mapper.at(sr.extra.f_type);
        }
        return make_ready_future<fs_type>(ret);
    });
}


future<file>
reactor::open_directory(sstring name) {
    return _thread_pool.submit<syscall_result<int>>([name] {
        return wrap_syscall<int>(::open(name.c_str(), O_DIRECTORY | O_CLOEXEC | O_RDONLY));
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
        return make_ready_future<file>(file(sr.result, file_open_options()));
    });
}

future<>
reactor::make_directory(sstring name) {
    return _thread_pool.submit<syscall_result<int>>([name = std::move(name)] {
        return wrap_syscall<int>(::mkdir(name.c_str(), S_IRWXU));
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
    });
}

future<>
reactor::touch_directory(sstring name) {
    return engine()._thread_pool.submit<syscall_result<int>>([name = std::move(name)] {
        return wrap_syscall<int>(::mkdir(name.c_str(), S_IRWXU));
    }).then([] (syscall_result<int> sr) {
        if (sr.error != EEXIST) {
            sr.throw_if_error();
        }
    });
}

future<>
posix_file_impl::flush(void) {
    ++engine()._fsyncs;
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

blockdev_file_impl::blockdev_file_impl(int fd, file_open_options options)
        : posix_file_impl(fd, options) {
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
posix_file_impl::size(void) {
    auto r = ::lseek(_fd, 0, SEEK_END);
    if (r == -1) {
        return make_exception_future<uint64_t>(std::system_error(errno, std::system_category()));
    }
    return make_ready_future<uint64_t>(r);
}

future<>
posix_file_impl::close() noexcept {
    auto closed = [fd = _fd] () noexcept {
        try {
            return engine()._thread_pool.submit<syscall_result<int>>([fd] {
                return wrap_syscall<int>(::close(fd));
            });
        } catch (...) {
            report_exception("Running ::close() in reactor thread, submission failed with exception", std::current_exception());
            return make_ready_future<syscall_result<int>>(wrap_syscall<int>(::close(fd)));
        }
    }();
    return closed.then([this] (syscall_result<int> sr) {
        _fd = -1;
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
    struct work {
        stream<directory_entry> s;
        unsigned current = 0;
        unsigned total = 0;
        bool eof = false;
        int error = 0;
        char buffer[8192];
    };

    // While it would be natural to use fdopendir()/readdir(),
    // our syscall thread pool doesn't support malloc(), which is
    // required for this to work.  So resort to using getdents()
    // instead.

    // From getdents(2):
    struct linux_dirent {
        unsigned long  d_ino;     /* Inode number */
        unsigned long  d_off;     /* Offset to next linux_dirent */
        unsigned short d_reclen;  /* Length of this linux_dirent */
        char           d_name[];  /* Filename (null-terminated) */
        /* length is actually (d_reclen - 2 -
                             offsetof(struct linux_dirent, d_name)) */
        /*
        char           pad;       // Zero padding byte
        char           d_type;    // File type (only since Linux
                                  // 2.6.4); offset is (d_reclen - 1)
         */
    };

    auto w = make_lw_shared<work>();
    auto ret = w->s.listen(std::move(next));
    w->s.started().then([w, this] {
        auto eofcond = [w] { return w->eof; };
        return do_until(eofcond, [w, this] {
            if (w->current == w->total) {
                return engine()._thread_pool.submit<syscall_result<long>>([w , this] () {
                    auto ret = ::syscall(__NR_getdents, _fd, reinterpret_cast<linux_dirent*>(w->buffer), sizeof(w->buffer));
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
            auto de = reinterpret_cast<linux_dirent*>(start);
            std::experimental::optional<directory_entry_type> type;
            switch (start[de->d_reclen - 1]) {
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
    auto ret = timer_settime(_steady_clock_timer, TIMER_ABSTIME, &its, NULL);
    throw_system_error_on(ret == -1);
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

void reactor::at_exit(std::function<future<> ()> func) {
    assert(!_stopping);
    _exit_funcs.push_back(std::move(func));
}

future<> reactor::run_exit_tasks() {
    _stopping = true;
    return do_for_each(_exit_funcs.rbegin(), _exit_funcs.rend(), [] (auto& func) {
        return func();
    });
}

void reactor::stop() {
    assert(engine()._id == 0);
    if (!_stopping) {
        run_exit_tasks().then([this] {
            do_with(semaphore(0), [this] (semaphore& sem) {
                for (unsigned i = 1; i < smp::count; i++) {
                    smp::submit_to<>(i, []() {
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

struct reactor::collectd_registrations {
    scollectd::registrations regs;
};

reactor::collectd_registrations
reactor::register_collectd_metrics() {
    return collectd_registrations{ {
            // queue_length     value:GAUGE:0:U
            // Absolute value of num tasks in queue.
            scollectd::add_polled_metric(scollectd::type_instance_id("reactor"
                    , scollectd::per_cpu_plugin_instance
                    , "queue_length", "tasks-pending")
                    , scollectd::make_typed(scollectd::data_type::GAUGE
                            , std::bind(&decltype(_pending_tasks)::size, &_pending_tasks))
            ),
            // total_operations value:DERIVE:0:U
            scollectd::add_polled_metric(scollectd::type_instance_id("reactor"
                    , scollectd::per_cpu_plugin_instance
                    , "total_operations", "tasks-processed")
                    , scollectd::make_typed(scollectd::data_type::DERIVE, _tasks_processed)
            ),
            // queue_length     value:GAUGE:0:U
            // Absolute value of num timers in queue.
            scollectd::add_polled_metric(scollectd::type_instance_id("reactor"
                    , scollectd::per_cpu_plugin_instance
                    , "queue_length", "timers-pending")
                    , scollectd::make_typed(scollectd::data_type::GAUGE
                            , std::bind(&decltype(_timers)::size, &_timers))
            ),
            scollectd::add_polled_metric(scollectd::type_instance_id("reactor"
                    , scollectd::per_cpu_plugin_instance
                    , "gauge", "load")
                    , scollectd::make_typed(scollectd::data_type::GAUGE,
                            [this] () -> uint32_t { return (1 - _load) * 100; })
            ),
            // total_operations value:DERIVE:0:U
            scollectd::add_polled_metric(scollectd::type_instance_id("reactor"
                    , scollectd::per_cpu_plugin_instance
                    , "total_operations", "aio-reads")
                    , scollectd::make_typed(scollectd::data_type::DERIVE, _aio_reads)
            ),
            scollectd::add_polled_metric(scollectd::type_instance_id("reactor"
                    , scollectd::per_cpu_plugin_instance
                    , "derive", "aio-read-bytes")
                    , scollectd::make_typed(scollectd::data_type::DERIVE, _aio_read_bytes)
            ),
            // total_operations value:DERIVE:0:U
            scollectd::add_polled_metric(scollectd::type_instance_id("reactor"
                    , scollectd::per_cpu_plugin_instance
                    , "total_operations", "aio-writes")
                    , scollectd::make_typed(scollectd::data_type::DERIVE, _aio_writes)
            ),
            scollectd::add_polled_metric(scollectd::type_instance_id("reactor"
                    , scollectd::per_cpu_plugin_instance
                    , "derive", "aio-write-bytes")
                    , scollectd::make_typed(scollectd::data_type::DERIVE, _aio_write_bytes)
            ),
            scollectd::add_polled_metric(scollectd::type_instance_id("reactor"
                    , scollectd::per_cpu_plugin_instance
                    , "gauge", "queued-io-requests")
                    , scollectd::make_typed(scollectd::data_type::GAUGE,
                        [this] { return _io_queue->queued_requests(); } )
            ),
            // total_operations value:DERIVE:0:U
            scollectd::add_polled_metric(scollectd::type_instance_id("reactor"
                    , scollectd::per_cpu_plugin_instance
                    , "total_operations", "fsyncs")
                    , scollectd::make_typed(scollectd::data_type::DERIVE, _fsyncs)
            ),
            // total_operations value:DERIVE:0:U
            scollectd::add_polled_metric(scollectd::type_instance_id("reactor"
                    , scollectd::per_cpu_plugin_instance
                    , "total_operations", "io-threaded-fallbacks")
                    , scollectd::make_typed(scollectd::data_type::DERIVE,
                            std::bind(&thread_pool::operation_count, &_thread_pool))
            ),
            scollectd::add_polled_metric(
                scollectd::type_instance_id("memory",
                    scollectd::per_cpu_plugin_instance,
                    "total_operations", "malloc"),
                scollectd::make_typed(scollectd::data_type::DERIVE,
                        [] { return memory::stats().mallocs(); })
            ),
            scollectd::add_polled_metric(
                scollectd::type_instance_id("memory",
                    scollectd::per_cpu_plugin_instance,
                    "total_operations", "free"),
                scollectd::make_typed(scollectd::data_type::DERIVE,
                        [] { return memory::stats().frees(); })
            ),
            scollectd::add_polled_metric(
                scollectd::type_instance_id("memory",
                    scollectd::per_cpu_plugin_instance,
                    "total_operations", "cross_cpu_free"),
                scollectd::make_typed(scollectd::data_type::DERIVE,
                        [] { return memory::stats().cross_cpu_frees(); })
            ),
            scollectd::add_polled_metric(
                scollectd::type_instance_id("memory",
                    scollectd::per_cpu_plugin_instance,
                    "objects", "malloc"),
                scollectd::make_typed(scollectd::data_type::GAUGE,
                        [] { return memory::stats().live_objects(); })
            ),
            scollectd::add_polled_metric(
                scollectd::type_instance_id("memory",
                    scollectd::per_cpu_plugin_instance,
                    "memory", "free_memory"),
                scollectd::make_typed(scollectd::data_type::GAUGE,
                        [] { return memory::stats().free_memory(); })
            ),
            scollectd::add_polled_metric(
                scollectd::type_instance_id("memory",
                    scollectd::per_cpu_plugin_instance,
                    "memory", "total_memory"),
                scollectd::make_typed(scollectd::data_type::GAUGE,
                        [] { return memory::stats().total_memory(); })
            ),
            scollectd::add_polled_metric(
                scollectd::type_instance_id("memory",
                    scollectd::per_cpu_plugin_instance,
                    "memory", "allocated_memory"),
                scollectd::make_typed(scollectd::data_type::GAUGE,
                        [] { return memory::stats().allocated_memory(); })
            ),
            scollectd::add_polled_metric(
                scollectd::type_instance_id("memory",
                    scollectd::per_cpu_plugin_instance,
                    "total_operations", "reclaims"),
                scollectd::make_typed(scollectd::data_type::DERIVE,
                        [] { return memory::stats().reclaims(); })
            ),
            scollectd::add_polled_metric(
                scollectd::type_instance_id("reactor",
                    scollectd::per_cpu_plugin_instance,
                    "total_operations", "logging_failures"),
                scollectd::make_typed(scollectd::data_type::GAUGE,
                        [] { return logging_failures; })
            ),
    } };
}

void reactor::run_tasks(circular_buffer<std::unique_ptr<task>>& tasks) {
    _task_quota_finished = false;
    future_avail_count = 0;
    while (!tasks.empty() && !_task_quota_finished) {
        auto tsk = std::move(tasks.front());
        tasks.pop_front();
        tsk->run();
        tsk.reset();
        ++_tasks_processed;
        std::atomic_signal_fence(std::memory_order_relaxed); // for _task_quota_finished flag
    }
}

void reactor::force_poll() {
    _task_quota_finished = true;
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
    if (now > _lowres_next_timeout) {
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

#ifndef HAVE_OSV

class reactor::io_pollfn final : public reactor::pollfn {
    reactor& _r;
public:
    io_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() override {
        return _r.process_io();
    }
    virtual bool try_enter_interrupt_mode() override {
        // aio cannot generate events if there are no inflight aios
        return _r._io_context_available.current() == reactor::max_aio;
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
    struct aligned_flag {
        std::atomic<bool> flag;
        char pad[63];
        bool try_lock() {
            return !flag.exchange(true, std::memory_order_relaxed);
        }
        void unlock() {
            flag.store(false, std::memory_order_relaxed);
        }
    };
    static aligned_flag _membarrier_lock;
public:
    smp_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() final override {
        return smp::poll_queues();
    }
    virtual bool try_enter_interrupt_mode() override {
        // systemwide_memory_barrier() is very slow if run concurrently,
        // so don't go to sleep if it is running now.
        if (!_membarrier_lock.try_lock()) {
            return false;
        }
        _r._sleeping.store(true, std::memory_order_relaxed);
        systemwide_memory_barrier();
        _membarrier_lock.unlock();
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

alignas(64) reactor::smp_pollfn::aligned_flag reactor::smp_pollfn::_membarrier_lock;

class reactor::epoll_pollfn final : public reactor::pollfn {
    reactor& _r;
public:
    epoll_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() final override {
        return _r.wait_and_process();
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
    pthread_kill(_thread_id, alarm_signal());
}

int reactor::run() {
    auto collectd_metrics = register_collectd_metrics();

#ifndef HAVE_OSV
    poller io_poller(std::make_unique<io_pollfn>(*this));
#endif

    poller sig_poller(std::make_unique<signal_pollfn>(*this));
    poller aio_poller(std::make_unique<aio_batch_submit_pollfn>(*this));
    poller batch_flush_poller(std::make_unique<batch_flush_pollfn>(*this));

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

    // Register smp queues poller
    std::experimental::optional<poller> smp_poller;
    if (smp::count > 1) {
        smp_poller = poller(std::make_unique<smp_pollfn>(*this));
    }

#ifndef HAVE_OSV
    _signals.handle_signal(alarm_signal(), [this] {
        complete_timers(_timers, _expired_timers, [this] {
            if (!_timers.empty()) {
                enable_timer(_timers.get_next_timeout());
            }
        });
    });
#endif

    poller drain_cross_cpu_freelist(std::make_unique<drain_cross_cpu_freelist_pollfn>());

    poller expire_lowres_timers(std::make_unique<lowres_timer_pollfn>(*this));

    using namespace std::chrono_literals;
    timer<lowres_clock> load_timer;
    steady_clock_type::rep idle_count = 0;
    auto idle_start = steady_clock_type::now(), idle_end = idle_start;
    load_timer.set_callback([this, &idle_count, &idle_start, &idle_end] () mutable {
        auto load = double(idle_count + (idle_end - idle_start).count()) / double(std::chrono::duration_cast<steady_clock_type::duration>(1s).count());
        load = std::min(load, 1.0);
        idle_count = 0;
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

    itimerspec its = {};
    auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(_task_quota).count();
    auto tv_nsec = nsec % 1'000'000'000;
    auto tv_sec = nsec / 1'000'000'000;
    its.it_value.tv_nsec = tv_nsec;
    its.it_value.tv_sec = tv_sec;
    its.it_interval = its.it_value;
    auto r = timer_settime(_task_quota_timer, 0, &its, nullptr);
    assert(r == 0);

    struct sigaction sa_task_quota = {};
    sa_task_quota.sa_handler = &reactor::clear_task_quota;
    sa_task_quota.sa_flags = SA_RESTART;
    r = sigaction(task_quota_signal(), &sa_task_quota, nullptr);
    assert(r == 0);

    bool idle = false;

    while (true) {
        run_tasks(_pending_tasks);
        if (_stopped) {
            load_timer.cancel();
            // Final tasks may include sending the last response to cpu 0, so run them
            while (!_pending_tasks.empty()) {
                run_tasks(_pending_tasks);
            }
            while (!_at_destroy_tasks.empty()) {
                run_tasks(_at_destroy_tasks);
            }
            smp::arrive_at_event_loop_end();
            if (_id == 0) {
                smp::join_all();
            }
            break;
        }

        if (!poll_once() && _pending_tasks.empty()) {
            idle_end = steady_clock_type::now();
            if (!idle) {
                idle_start = idle_end;
                idle = true;
            }
            _mm_pause();
            if (idle_end - idle_start > _max_poll_time) {
                sleep();
                // We may have slept for a while, so freshen idle_end
                idle_end = steady_clock_type::now();
            }
        } else {
            if (idle) {
                idle_count += (idle_end - idle_start).count();
                idle_start = idle_end;
                idle = false;
            }
        }
    }
    // To prevent ordering issues from rising, destroy the I/O queue explicitly at this point.
    // This is needed because the reactor is destroyed from the thread_local destructors. If
    // the I/O queue happens to use any other infrastructure that is also kept this way (for
    // instance, collectd), we will not have any way to guarantee who is destroyed first.
    my_io_queue.reset(nullptr);
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

class reactor::poller::registration_task : public task {
private:
    poller* _p;
public:
    explicit registration_task(poller* p) : _p(p) {}
    virtual void run() noexcept override {
        if (_p) {
            engine().register_poller(_p->_pollfn.get());
            _p->_registration_task = nullptr;
        }
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
    virtual void run() noexcept override {
        engine().unregister_poller(_p.get());
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
        auto events = evt.events & (EPOLLIN | EPOLLOUT);
        auto events_to_remove = events & ~pfd->events_requested;
        complete_epoll_event(*pfd, &pollable_fd_state::pollin, events, EPOLLIN);
        complete_epoll_event(*pfd, &pollable_fd_state::pollout, events, EPOLLOUT);
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

void syscall_work_queue::submit_item(syscall_work_queue::work_item* item) {
    _queue_has_room.wait().then([this, item] {
        _pending.push(item);
        _start_eventfd.signal(1);
    });
}

void syscall_work_queue::complete() {
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
}

smp_message_queue::smp_message_queue(reactor* from, reactor* to)
    : _pending(to)
    , _completed(from)
{
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

void smp_message_queue::submit_item(smp_message_queue::work_item* item) {
    _tx.a.pending_fifo.push_back(item);
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
    // start prefecthing first item before popping the rest to overlap memory
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
    move_pending();
}

size_t smp_message_queue::process_incoming() {
    auto nr = process_queue<prefetch_cnt>(_pending, [this] (work_item* wi) {
        wi->process().then([this, wi] {
            respond(wi);
        });
    });
    _received += nr;
    _last_rcv_batch = nr;
    return nr;
}

void smp_message_queue::start(unsigned cpuid) {
    _tx.init();
    char instance[10];
    std::snprintf(instance, sizeof(instance), "%u-%u", engine().cpu_id(), cpuid);
    _collectd_regs = scollectd::registrations({
            // queue_length     value:GAUGE:0:U
            // Absolute value of num packets in last tx batch.
            scollectd::add_polled_metric(scollectd::type_instance_id("smp"
                    , instance
                    , "queue_length", "send-batch")
            , scollectd::make_typed(scollectd::data_type::GAUGE, _last_snt_batch)
            ),
            scollectd::add_polled_metric(scollectd::type_instance_id("smp"
                    , instance
                    , "queue_length", "receive-batch")
            , scollectd::make_typed(scollectd::data_type::GAUGE, _last_rcv_batch)
            ),
            scollectd::add_polled_metric(scollectd::type_instance_id("smp"
                    , instance
                    , "queue_length", "complete-batch")
            , scollectd::make_typed(scollectd::data_type::GAUGE, _last_cmpl_batch)
            ),
            scollectd::add_polled_metric(scollectd::type_instance_id("smp"
                    , instance
                    , "queue_length", "send-queue-length")
            , scollectd::make_typed(scollectd::data_type::GAUGE, _current_queue_length)
            ),
            // total_operations value:DERIVE:0:U
            scollectd::add_polled_metric(scollectd::type_instance_id("smp"
                    , instance
                    , "total_operations", "received-messages")
            , scollectd::make_typed(scollectd::data_type::DERIVE, _received)
            ),
            // total_operations value:DERIVE:0:U
            scollectd::add_polled_metric(scollectd::type_instance_id("smp"
                    , instance
                    , "total_operations", "sent-messages")
            , scollectd::make_typed(scollectd::data_type::DERIVE, _sent)
            ),
            // total_operations value:DERIVE:0:U
            scollectd::add_polled_metric(scollectd::type_instance_id("smp"
                    , instance
                    , "total_operations", "completed-messages")
            , scollectd::make_typed(scollectd::data_type::DERIVE, _compl)
            ),
    });
}

/* not yet implemented for OSv. TODO: do the notification like we do class smp. */
#ifndef HAVE_OSV
thread_pool::thread_pool() : _worker_thread([this] { work(); }), _notify(pthread_self()) {
    engine()._signals.handle_signal(SIGUSR1, [this] { inter_thread_wq.complete(); });
}

void thread_pool::work() {
    sigset_t mask;
    sigfillset(&mask);
    auto r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
    throw_system_error_on(r == -1);
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
        pthread_kill(_notify, SIGUSR1);
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

bool operator==(const ::sockaddr_in a, const ::sockaddr_in b) {
    return (a.sin_addr.s_addr == b.sin_addr.s_addr) && (a.sin_port == b.sin_port);
}

void network_stack_registry::register_stack(sstring name,
        boost::program_options::options_description opts,
        std::function<future<std::unique_ptr<network_stack>> (options opts)> create, bool make_default) {
    _map()[name] = std::move(create);
    options_description().add(opts);
    if (make_default) {
        _default() = name;
    }
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
        throw std::runtime_error(sprint("network stack %s not registered", name));
    }
    return _map()[name](opts);
}

network_stack_registrator::network_stack_registrator(sstring name,
        boost::program_options::options_description opts,
        std::function<future<std::unique_ptr<network_stack>>(options opts)> factory,
        bool make_default) {
    network_stack_registry::register_stack(name, opts, factory, make_default);
}

boost::program_options::options_description
reactor::get_options_description() {
    namespace bpo = boost::program_options;
    bpo::options_description opts("Core options");
    auto net_stack_names = network_stack_registry::list();
    opts.add_options()
        ("network-stack", bpo::value<std::string>(),
                sprint("select network stack (valid values: %s)",
                        format_separated(net_stack_names.begin(), net_stack_names.end(), ", ")).c_str())
        ("no-handle-interrupt", "ignore SIGINT (for gdb)")
        ("poll-mode", "poll continuously (100% cpu use)")
        ("task-quota-ms", bpo::value<double>()->default_value(2.0), "Max time (ms) between polls")
        ("relaxed-dma", "allow using buffered I/O if DMA is not available (reduces performance)");
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
#ifdef HAVE_HWLOC
        ("num-io-queues", bpo::value<unsigned>(), "Number of IO queues. Each IO unit will be responsible for a fraction of the IO requests. Defaults to the number of threads")
        ("max-io-requests", bpo::value<unsigned>(), "Maximum amount of concurrent requests to be sent to the disk. Defaults to 128 times the number of IO queues")
#else
        ("max-io-requests", bpo::value<unsigned>(), "Maximum amount of concurrent requests to be sent to the disk. Defaults to 128 times the number of processors")
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

std::vector<smp::thread_adaptor> smp::_threads;
std::experimental::optional<boost::barrier> smp::_all_event_loops_done;
std::vector<reactor*> smp::_reactors;
smp_message_queue** smp::_qs;
std::thread::id smp::_tmain;
unsigned smp::count = 1;

void smp::start_all_queues()
{
    for (unsigned c = 0; c < count; c++) {
        if (c != engine().cpu_id()) {
            _qs[c][engine().cpu_id()].start(c);
        }
    }
}

#ifdef HAVE_DPDK

int dpdk_thread_adaptor(void* f)
{
    (*static_cast<std::function<void ()>*>(f))();
    return 0;
}

void smp::join_all()
{
    rte_eal_mp_wait_lcore();
}

void smp::pin(unsigned cpu_id) {
}
#else
void smp::join_all()
{
    for (auto&& t: smp::_threads) {
        t.join();
    }
}

void smp::pin(unsigned cpu_id) {
    pin_this_thread(cpu_id);
}
#endif

void smp::arrive_at_event_loop_end() {
    if (_all_event_loops_done) {
        _all_event_loops_done->wait();
    }
}

void smp::allocate_reactor() {
    assert(!reactor_holder);

    // we cannot just write "local_engin = new reactor" since reactor's constructor
    // uses local_engine
    void *buf;
    int r = posix_memalign(&buf, 64, sizeof(reactor));
    assert(r == 0);
    local_engine = reinterpret_cast<reactor*>(buf);
    new (buf) reactor;
    reactor_holder.reset(local_engine);
}

void smp::cleanup() {
    smp::_threads = std::vector<thread_adaptor>();
}

void smp::configure(boost::program_options::variables_map configuration)
{
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
#ifdef HAVE_DPDK
        if (configuration.count("hugepages") &&
            !configuration["network-stack"].as<std::string>().compare("native") &&
            configuration.count("dpdk-pmd")) {
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
    std::experimental::optional<std::string> hugepages_path;
    if (configuration.count("hugepages")) {
        hugepages_path = configuration["hugepages"].as<std::string>();
    }

    rc.cpus = smp::count;
    rc.cpu_set = std::move(cpu_set);
    if (configuration.count("max-io-requests")) {
        rc.max_io_requests = configuration["max-io-requests"].as<unsigned>();
    }

    if (configuration.count("num-io-queues")) {
        rc.io_queues = configuration["num-io-queues"].as<unsigned>();
    }

    auto resources = resource::allocate(rc);
    std::vector<resource::cpu> allocations = std::move(resources.cpus);
    smp::pin(allocations[0].cpu_id);
    memory::configure(allocations[0].mem, hugepages_path);

#ifdef HAVE_DPDK
    dpdk::eal::cpuset cpus;
    for (auto&& a : allocations) {
        cpus[a.cpu_id] = true;
    }
    dpdk::eal::init(cpus, configuration);
#endif

    // Better to put it into the smp class, but at smp construction time
    // correct smp::count is not known.
    static boost::barrier reactors_registered(smp::count);
    static boost::barrier smp_queues_constructed(smp::count);
    static boost::barrier inited(smp::count);

    auto io_info = std::move(resources.io_queues);

    std::vector<io_queue*> all_io_queues;
    all_io_queues.resize(io_info.coordinators.size());
    io_queue::fill_shares_array();

    auto alloc_io_queue = [io_info, &all_io_queues] (unsigned shard) {
        auto cid = io_info.shard_to_coordinator[shard];
        int vec_idx = 0;
        for (auto& coordinator: io_info.coordinators) {
            if (coordinator.id != cid) {
                vec_idx++;
                continue;
            }
            if (shard == cid) {
                all_io_queues[vec_idx] = new io_queue(coordinator.id, coordinator.capacity, io_info.shard_to_coordinator);
            }
            return vec_idx;
        }
        assert(0); // Impossible
    };

    auto assign_io_queue = [&all_io_queues] (shard_id id, int queue_idx) {
        if (all_io_queues[queue_idx]->coordinator() == id) {
            engine().my_io_queue.reset(all_io_queues[queue_idx]);
        }
        engine()._io_queue = all_io_queues[queue_idx];
        engine()._io_coordinator = all_io_queues[queue_idx]->coordinator();
    };

    _all_event_loops_done.emplace(smp::count);

    unsigned i;
    for (i = 1; i < smp::count; i++) {
        auto allocation = allocations[i];
        _threads.emplace_back([configuration, hugepages_path, i, allocation, assign_io_queue, alloc_io_queue] {
            smp::pin(allocation.cpu_id);
            memory::configure(allocation.mem, hugepages_path);
            sigset_t mask;
            sigfillset(&mask);
            auto r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
            throw_system_error_on(r == -1);
            allocate_reactor();
            engine()._id = i;
            _reactors[i] = &engine();
            auto queue_idx = alloc_io_queue(i);
            reactors_registered.wait();
            smp_queues_constructed.wait();
            start_all_queues();
            assign_io_queue(i, queue_idx);
            inited.wait();
            engine().configure(configuration);
            engine().run();
        });
    }

    allocate_reactor();
    _reactors[0] = &engine();
    auto queue_idx = alloc_io_queue(0);

#ifdef HAVE_DPDK
    auto it = _threads.begin();
    RTE_LCORE_FOREACH_SLAVE(i) {
        rte_eal_remote_launch(dpdk_thread_adaptor, static_cast<void*>(&*(it++)), i);
    }
#endif

    reactors_registered.wait();
    smp::_qs = new smp_message_queue* [smp::count];
    for(unsigned i = 0; i < smp::count; i++) {
        smp::_qs[i] = reinterpret_cast<smp_message_queue*>(operator new[] (sizeof(smp_message_queue) * smp::count));
        for (unsigned j = 0; j < smp::count; ++j) {
            new (&smp::_qs[i][j]) smp_message_queue(_reactors[j], _reactors[i]);
        }
    }
    smp_queues_constructed.wait();
    start_all_queues();
    assign_io_queue(0, queue_idx);
    inited.wait();

    engine().configure(configuration);
    engine()._lowres_clock = std::make_unique<lowres_clock>();
}

__thread size_t future_avail_count = 0;

__thread reactor* local_engine;

class reactor_notifier_epoll : public reactor_notifier {
    writeable_eventfd _write;
    readable_eventfd _read;
public:
    reactor_notifier_epoll()
        : _write()
        , _read(_write.read_side()) {
    }
    virtual future<> wait() override {
        // convert _read.wait(), a future<size_t>, to a future<>:
        return _read.wait().then([this] (size_t ignore) {
            return make_ready_future<>();
        });
    }
    virtual void signal() override {
        _write.signal(1);
    }
};

std::unique_ptr<reactor_notifier>
reactor_backend_epoll::make_reactor_notifier() {
    return std::make_unique<reactor_notifier_epoll>();
}

#ifdef HAVE_OSV
class reactor_notifier_osv :
        public reactor_notifier, private osv::newpoll::pollable {
    promise<> _pr;
    // TODO: pollable should probably remember its poller, so we shouldn't
    // need to keep another copy of this pointer
    osv::newpoll::poller *_poller = nullptr;
    bool _needed = false;
public:
    virtual future<> wait() override {
        return engine().notified(this);
    }
    virtual void signal() override {
        wake();
    }
    virtual void on_wake() override {
        _pr.set_value();
        _pr = promise<>();
        // We try to avoid del()/add() ping-pongs: After an one occurance of
        // the event, we don't del() but rather set needed=false. We guess
        // the future's continuation (scheduler by _pr.set_value() above)
        // will make the pollable needed again. Only if we reach this callback
        // a second time, and needed is still false, do we finally del().
        if (!_needed) {
            _poller->del(this);
            _poller = nullptr;

        }
        _needed = false;
    }

    void enable(osv::newpoll::poller &poller) {
        _needed = true;
        if (_poller == &poller) {
            return;
        }
        assert(!_poller); // don't put same pollable on multiple pollers!
        _poller = &poller;
        _poller->add(this);
    }

    virtual ~reactor_notifier_osv() {
        if (_poller) {
            _poller->del(this);
        }
    }

    friend class reactor_backend_osv;
};

std::unique_ptr<reactor_notifier>
reactor_backend_osv::make_reactor_notifier() {
    return std::make_unique<reactor_notifier_osv>();
}
#endif


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
reactor_backend_osv::notified(reactor_notifier *notifier) {
    // reactor_backend_osv::make_reactor_notifier() generates a
    // reactor_notifier_osv, so we only can work on such notifiers.
    reactor_notifier_osv *n = dynamic_cast<reactor_notifier_osv *>(notifier);
    if (n->read()) {
        return make_ready_future<>();
    }
    n->enable(_poller);
    return n->_pr.get_future();
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

void report_exception(std::experimental::string_view message, std::exception_ptr eptr) noexcept {
    try {
#ifndef __GNUC__
    std::cerr << message << ".\n";
#else
    try {
        std::rethrow_exception(eptr);
    } catch(...) {
        auto tp = abi::__cxa_current_exception_type();
        std::cerr << message;
        if (tp) {
            int status;
            char *demangled = abi::__cxa_demangle(tp->name(), 0, 0, &status);
            std::cerr << " of type '";
            if (status == 0) {
                std::cerr << demangled;
                free(demangled);
            } else {
                std::cerr << tp->name();
            }
            std::cerr << "'";
        } else {
            std::cerr << " of unknown type";
        }
        // Print more information on some known exception types
        try {
            throw;
        } catch(const std::system_error &e) {
            std::cerr << ": Error " << e.code() << " (" << e.code().message() << ")\n";
        } catch(const std::exception& e) {
            std::cerr << ": " << e.what() << "\n";
        } catch(...) {
            std::cerr << ".\n";
        }
    }
#endif
    } catch (...) {
        ++logging_failures;
    }
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
    report_exception("WARNING: exceptional future ignored", eptr);
}

future<> check_direct_io_support(sstring path) {
    struct w {
        sstring path;
        open_flags flags;
        std::function<future<>()> cleanup;

        static w parse(sstring path, std::experimental::optional<directory_entry_type> type) {
            if (!type) {
                throw std::invalid_argument(sprint("Could not open file at %s. Make sure it exists", path));
            }

            if (type == directory_entry_type::directory) {
                auto fpath = path + "/.o_direct_test";
                return w{fpath, open_flags::wo | open_flags::create | open_flags::truncate, [fpath] { return remove_file(fpath); }};
            } else if ((type == directory_entry_type::regular) || (type == directory_entry_type::link)) {
                return w{path, open_flags::ro, [] { return make_ready_future<>(); }};
            } else {
                throw std::invalid_argument(sprint("%s neither a directory nor file. Can't be opened with O_DIRECT", path));
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
                    report_exception(sprint("Could not open file at %s. Does your filesystem support O_DIRECT?", path), std::current_exception());
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

future<uint64_t> file_size(sstring name) {
    return engine().file_size(name);
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

future<connected_socket> connect(socket_address sa, socket_address local) {
    return engine().connect(sa, local);
}

void reactor::add_high_priority_task(std::unique_ptr<task>&& t) {
    _pending_tasks.push_front(std::move(t));
    // break .then() chains
    future_avail_count = max_inlined_continuations - 1;
}

static
bool
virtualized() {
    return boost::filesystem::exists("/sys/hypervisor/type");
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
    schedule(make_task([p = std::move(p)] () mutable {
        engine().force_poll();
        p.set_value();
    }));
    return f;
}

void add_to_flush_poller(output_stream<char>* os) {
    engine()._flush_batching.emplace_back(os);
}

network_stack_registrator nsr_posix{"posix",
    boost::program_options::options_description(),
    [](boost::program_options::variables_map ops) {
        return smp::main_thread() ? posix_network_stack::create(ops) : posix_ap_network_stack::create(ops);
    },
    true
};
