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

#ifdef SEASTAR_MODULE
module;
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <coroutine>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <regex>
#include <thread>

#include <grp.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <netinet/in.h>
#include <boost/lexical_cast.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/constants.hpp>
#include <boost/algorithm/string/find_iterator.hpp>
#include <boost/algorithm/string/finder.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/numeric.hpp>
#include <boost/range/algorithm/sort.hpp>
#include <boost/range/algorithm/remove_if.hpp>
#include <boost/range/algorithm/find_if.hpp>
#include <boost/algorithm/clamp.hpp>
#include <boost/version.hpp>
#include <dirent.h>
#define __user /* empty */  // for xfs includes, below
#include <linux/types.h> // for xfs, below
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <xfs/linux.h>
/*
 * With package xfsprogs-devel >= 5.14.1, `fallthrough` has defined to
 * fix compilation warning in header <xfs/linux.h>,
 * (see: https://git.kernel.org/pub/scm/fs/xfs/xfsprogs-dev.git/commit/?id=df9c7d8d8f3ed0785ed83e7fd0c7ddc92cbfbe15)
 * There is a confliction with c++ keyword `fallthrough`, so undefine fallthrough here.
 */
#undef fallthrough
#define min min    /* prevent xfs.h from defining min() as a macro */
#include <xfs/xfs.h>
#undef min
#include <fmt/ostream.h>
#include <fmt/ranges.h>

#ifdef SEASTAR_SHUFFLE_TASK_QUEUE
#include <random>
#endif

#include <sys/mman.h>
#include <sys/utsname.h>
#include <linux/falloc.h>
#ifdef SEASTAR_HAVE_SYSTEMTAP_SDT
#include <sys/sdt.h>
#else
#define STAP_PROBE(provider, name)
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

#ifdef SEASTAR_HAVE_DPDK
#include <rte_lcore.h>
#include <rte_launch.h>
#endif
#ifdef __GNUC__
#include <iostream>
#include <system_error>
#include <cxxabi.h>
#endif

#include <yaml-cpp/yaml.h>

#ifdef SEASTAR_TASK_HISTOGRAM
#include <typeinfo>
#endif

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/abort_on_ebadf.hh>
#include <seastar/core/alien.hh>
#include <seastar/core/exception_hacks.hh>
#include <seastar/core/execution_stage.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/make_task.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/prefetch.hh>
#include <seastar/core/print.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/report_exception.hh>
#include <seastar/core/resource.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/scheduling_specific.hh>
#include <seastar/core/signal.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/smp_options.hh>
#include <seastar/core/stall_sampler.hh>
#include <seastar/core/systemwide_memory_barrier.hh>
#include <seastar/core/task.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/thread_cputime_clock.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/core/internal/buffer_allocator.hh>
#include <seastar/core/internal/io_desc.hh>
#include <seastar/core/internal/uname.hh>
#include <seastar/core/internal/stall_detector.hh>
#include <seastar/core/internal/run_in_background.hh>
#include <seastar/net/native-stack.hh>
#include <seastar/net/packet.hh>
#include <seastar/net/posix-stack.hh>
#include <seastar/net/stack.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/util/conversions.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/log.hh>
#include <seastar/util/memory_diagnostics.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/util/print_safe.hh>
#include <seastar/util/process.hh>
#include <seastar/util/read_first_line.hh>
#include <seastar/util/spinlock.hh>
#include <seastar/util/internal/iovec_utils.hh>
#include <seastar/util/internal/magic.hh>
#include "core/reactor_backend.hh"
#include "core/syscall_result.hh"
#include "core/thread_pool.hh"
#include "syscall_work_queue.hh"
#include "cgroup.hh"
#ifdef SEASTAR_HAVE_DPDK
#include <seastar/core/dpdk_rte.hh>
#endif
#endif // SEASTAR_MODULE
#include <seastar/util/assert.hh>

namespace seastar {

static_assert(posix::shutdown_mask(SHUT_RD) == posix::rcv_shutdown);
static_assert(posix::shutdown_mask(SHUT_WR) == posix::snd_shutdown);
static_assert(posix::shutdown_mask(SHUT_RDWR) == (posix::snd_shutdown | posix::rcv_shutdown));

struct mountpoint_params {
    std::string mountpoint = "none";
    uint64_t read_bytes_rate = std::numeric_limits<uint64_t>::max();
    uint64_t write_bytes_rate = std::numeric_limits<uint64_t>::max();
    uint64_t read_req_rate = std::numeric_limits<uint64_t>::max();
    uint64_t write_req_rate = std::numeric_limits<uint64_t>::max();
    uint64_t read_saturation_length = std::numeric_limits<uint64_t>::max();
    uint64_t write_saturation_length = std::numeric_limits<uint64_t>::max();
    bool duplex = false;
    float rate_factor = 1.0;
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
        if (node["read_saturation_length"]) {
            mp.read_saturation_length = parse_memory_size(node["read_saturation_length"].as<std::string>());
        }
        if (node["write_saturation_length"]) {
            mp.write_saturation_length = parse_memory_size(node["write_saturation_length"].as<std::string>());
        }
        if (node["duplex"]) {
            mp.duplex = node["duplex"].as<bool>();
        }
        if (node["rate_factor"]) {
            mp.rate_factor = node["rate_factor"].as<float>();
        }
        return true;
    }
};
}

namespace seastar {

seastar::logger seastar_logger("seastar");
seastar::logger sched_logger("scheduler");

shard_id reactor::cpu_id() const {
    SEASTAR_ASSERT(_id == this_shard_id());
    return _id;
}

void reactor::update_shares_for_queues(internal::priority_class pc, uint32_t shares) {
    for (auto&& q : _io_queues) {
        q.second->update_shares_for_class(pc, shares);
    }
}

future<> reactor::update_bandwidth_for_queues(internal::priority_class pc, uint64_t bandwidth) {
    return smp::invoke_on_all([pc, bandwidth = bandwidth / _num_io_groups] {
        return parallel_for_each(engine()._io_queues, [pc, bandwidth] (auto& queue) {
            return queue.second->update_bandwidth_for_class(pc, bandwidth);
        });
    });
}

void reactor::rename_queues(internal::priority_class pc, sstring new_name) {
    for (auto&& queue : _io_queues) {
        queue.second->rename_priority_class(pc, new_name);
    }
}

future<std::tuple<pollable_fd, socket_address>>
reactor::do_accept(pollable_fd_state& listenfd) {
    return readable_or_writeable(listenfd).then([this, &listenfd] () mutable {
        socket_address sa;
        listenfd.maybe_no_more_recv();
        auto maybe_fd = listenfd.fd.try_accept(sa, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (!maybe_fd) {
            // We speculated that we will have an another connection, but got a false
            // positive. Try again without speculation.
            return do_accept(listenfd);
        }
        // Speculate that there is another connection on this listening socket, to avoid
        // a task-quota delay. Usually this will fail, but accept is a rare-enough operation
        // that it is worth the false positive in order to withstand a connection storm
        // without having to accept at a rate of 1 per task quota.
        listenfd.speculate_epoll(EPOLLIN);
        pollable_fd pfd(std::move(*maybe_fd), pollable_fd::speculation(EPOLLOUT));
        return make_ready_future<std::tuple<pollable_fd, socket_address>>(std::make_tuple(std::move(pfd), std::move(sa)));
    });
}

future<> reactor::do_connect(pollable_fd_state& pfd, socket_address& sa) {
    pfd.fd.connect(sa.u.sa, sa.length());
    return pfd.writeable().then([&pfd]() mutable {
        auto err = pfd.fd.getsockopt<int>(SOL_SOCKET, SO_ERROR);
        if (err != 0) {
            throw std::system_error(err, std::system_category());
        }
        return make_ready_future<>();
    });
}

future<size_t>
reactor::do_read(pollable_fd_state& fd, void* buffer, size_t len) {
    return readable(fd).then([this, &fd, buffer, len] () mutable {
        auto r = fd.fd.read(buffer, len);
        if (!r) {
            return do_read(fd, buffer, len);
        }
        if (size_t(*r) == len) {
            fd.speculate_epoll(EPOLLIN);
        }
        return make_ready_future<size_t>(*r);
    });
}

future<temporary_buffer<char>>
reactor::do_read_some(pollable_fd_state& fd, internal::buffer_allocator* ba) {
    return fd.readable().then([this, &fd, ba] {
        auto buffer = ba->allocate_buffer();
        auto r = fd.fd.read(buffer.get_write(), buffer.size());
        if (!r) {
            // Speculation failure, try again with real polling this time
            // Note we release the buffer and will reallocate it when poll
            // completes.
            return do_read_some(fd, ba);
        }
        if (size_t(*r) == buffer.size()) {
            fd.speculate_epoll(EPOLLIN);
        }
        buffer.trim(*r);
        return make_ready_future<temporary_buffer<char>>(std::move(buffer));
    });
}

future<size_t>
reactor::do_recvmsg(pollable_fd_state& fd, const std::vector<iovec>& iov) {
    return readable(fd).then([this, &fd, iov = iov] () mutable {
        ::msghdr mh = {};
        mh.msg_iov = &iov[0];
        mh.msg_iovlen = iov.size();
        auto r = fd.fd.recvmsg(&mh, 0);
        if (!r) {
            return do_recvmsg(fd, iov);
        }
        if (size_t(*r) == internal::iovec_len(iov)) {
            fd.speculate_epoll(EPOLLIN);
        }
        return make_ready_future<size_t>(*r);
    });
}

future<size_t>
reactor::do_send(pollable_fd_state& fd, const void* buffer, size_t len) {
    return writeable(fd).then([this, &fd, buffer, len] () mutable {
        auto r = fd.fd.send(buffer, len, MSG_NOSIGNAL);
        if (!r) {
            return do_send(fd, buffer, len);
        }
        if (size_t(*r) == len) {
            fd.speculate_epoll(EPOLLOUT);
        }
        return make_ready_future<size_t>(*r);
    });
}

future<size_t>
reactor::do_sendmsg(pollable_fd_state& fd, net::packet& p) {
    return writeable(fd).then([this, &fd, &p] () mutable {
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
        mh.msg_iovlen = std::min<size_t>(p.nr_frags(), IOV_MAX);
        auto r = fd.fd.sendmsg(&mh, MSG_NOSIGNAL);
        if (!r) {
            return do_sendmsg(fd, p);
        }
        if (size_t(*r) == p.len()) {
            fd.speculate_epoll(EPOLLOUT);
        }
        return make_ready_future<size_t>(*r);
    });
}

future<>
reactor::send_all_part(pollable_fd_state& fd, const void* buffer, size_t len, size_t completed) {
    if (completed == len) {
        return make_ready_future<>();
    } else {
        return _backend->send(fd, static_cast<const char*>(buffer) + completed, len - completed).then(
                [&fd, buffer, len, completed, this] (size_t part) mutable {
            return send_all_part(fd, buffer, len, completed + part);
        });
    }
}


future<temporary_buffer<char>>
reactor::do_recv_some(pollable_fd_state& fd, internal::buffer_allocator* ba) {
    return fd.readable().then([this, &fd, ba] {
        auto buffer = ba->allocate_buffer();
        auto r = fd.fd.recv(buffer.get_write(), buffer.size(), MSG_DONTWAIT);
        if (!r) {
            return do_recv_some(fd, ba);
        }
        if (size_t(*r) == buffer.size()) {
            fd.speculate_epoll(EPOLLIN);
        }
        buffer.trim(*r);
        return make_ready_future<temporary_buffer<char>>(std::move(buffer));
    });
}

future<>
reactor::send_all(pollable_fd_state& fd, const void* buffer, size_t len) {
    SEASTAR_ASSERT(len);
    return send_all_part(fd, buffer, len, 0);
}

future<size_t> pollable_fd_state::read_some(char* buffer, size_t size) {
    return engine()._backend->read(*this, buffer, size);
}

future<size_t> pollable_fd_state::read_some(uint8_t* buffer, size_t size) {
    return engine()._backend->read(*this, buffer, size);
}

future<size_t> pollable_fd_state::read_some(const std::vector<iovec>& iov) {
    return engine()._backend->recvmsg(*this, iov);
}

future<temporary_buffer<char>> pollable_fd_state::read_some(internal::buffer_allocator* ba) {
    return engine()._backend->read_some(*this, ba);
}

future<size_t> pollable_fd_state::write_some(net::packet& p) {
    return engine()._backend->sendmsg(*this, p);
}

future<> pollable_fd_state::write_all(const char* buffer, size_t size) {
    return engine().send_all(*this, buffer, size);
}

future<> pollable_fd_state::write_all(const uint8_t* buffer, size_t size) {
    return engine().send_all(*this, buffer, size);
}

future<> pollable_fd_state::write_all(net::packet& p) {
    return write_some(p).then([this, &p] (size_t size) {
        if (p.len() == size) {
            return make_ready_future<>();
        }
        p.trim_front(size);
        return write_all(p);
    });
}

future<> pollable_fd_state::readable() {
    return engine().readable(*this);
}

future<> pollable_fd_state::writeable() {
    return engine().writeable(*this);
}

future<> pollable_fd_state::poll_rdhup() {
    return engine().poll_rdhup(*this);
}

future<> pollable_fd_state::readable_or_writeable() {
    return engine().readable_or_writeable(*this);
}

future<std::tuple<pollable_fd, socket_address>> pollable_fd_state::accept() {
    return engine()._backend->accept(*this);
}

future<> pollable_fd_state::connect(socket_address& sa) {
    return engine()._backend->connect(*this, sa);
}

future<temporary_buffer<char>> pollable_fd_state::recv_some(internal::buffer_allocator* ba) {
    maybe_no_more_recv();
    return engine()._backend->recv_some(*this, ba);
}

future<size_t> pollable_fd_state::recvmsg(struct msghdr *msg) {
    maybe_no_more_recv();
    return engine().readable(*this).then([this, msg] {
        auto r = fd.recvmsg(msg, 0);
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
        speculate_epoll(EPOLLIN);
        return make_ready_future<size_t>(*r);
    });
}

future<size_t> pollable_fd_state::sendmsg(struct msghdr* msg) {
    maybe_no_more_send();
    return engine().writeable(*this).then([this, msg] () mutable {
        auto r = fd.sendmsg(msg, 0);
        if (!r) {
            return sendmsg(msg);
        }
        // For UDP this will always speculate. We can't know if there's room
        // or not, but most of the time there should be so the cost of mis-
        // speculation is amortized.
        if (size_t(*r) == internal::iovec_len(msg->msg_iov, msg->msg_iovlen)) {
            speculate_epoll(EPOLLOUT);
        }
        return make_ready_future<size_t>(*r);
    });
}

future<size_t> pollable_fd_state::sendto(socket_address addr, const void* buf, size_t len) {
    maybe_no_more_send();
    return engine().writeable(*this).then([this, buf, len, addr] () mutable {
        auto r = fd.sendto(addr, buf, len, 0);
        if (!r) {
            return sendto(std::move(addr), buf, len);
        }
        // See the comment about speculation in sendmsg().
        if (size_t(*r) == len) {
            speculate_epoll(EPOLLOUT);
        }
        return make_ready_future<size_t>(*r);
    });
}

namespace internal {

#ifdef SEASTAR_BUILD_SHARED_LIBS
const preemption_monitor*& get_need_preempt_var() {
    static preemption_monitor bootstrap_preemption_monitor;
    static thread_local const preemption_monitor* g_need_preempt = &bootstrap_preemption_monitor;
    return g_need_preempt;
}
#endif

void set_need_preempt_var(const preemption_monitor* np) {
    get_need_preempt_var() = np;
}

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

void task_histogram_add_task(const task& t) {
    this_thread_task_histogram.add(t);
}
#else
void task_histogram_add_task(const task& t) {
}
#endif

}

using namespace std::chrono_literals;
namespace fs = std::filesystem;

using namespace net;

using namespace internal::linux_abi;

std::atomic<manual_clock::rep> manual_clock::_now;

// Base version where this works; some filesystems were only fixed later, so
// this value is mixed in with filesystem-provided values later.
bool aio_nowait_supported = internal::kernel_uname().whitelisted({"4.13"});

static bool sched_debug() {
    return false;
}

template <typename... Args>
void
#if SEASTAR_LOGGER_COMPILE_TIME_FMT
sched_print(fmt::format_string<Args...> fmt, Args&&... args) {
#else
sched_print(const char* fmt, Args&&... args) {
#endif
    if (sched_debug()) {
        sched_logger.trace(fmt, std::forward<Args>(args)...);
    }
}

static std::atomic<bool> abort_on_ebadf = { false };

void set_abort_on_ebadf(bool do_abort) {
    abort_on_ebadf.store(do_abort);
}

bool is_abort_on_ebadf_enabled() {
    return abort_on_ebadf.load();
}

timespec to_timespec(steady_clock_type::time_point t) {
    using ns = std::chrono::nanoseconds;
    auto n = std::chrono::duration_cast<ns>(t.time_since_epoch()).count();
    return { n / 1'000'000'000, n % 1'000'000'000 };
}

void lowres_clock::update() noexcept {
    lowres_clock::_now = lowres_clock::time_point(std::chrono::steady_clock::now().time_since_epoch());
    lowres_system_clock::_now = lowres_system_clock::time_point(std::chrono::system_clock::now().time_since_epoch());
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
void timer<Clock>::arm(time_point until, std::optional<duration> period) noexcept {
    arm_state(until, period);
    engine().add_timer(this);
}

template <typename Clock>
inline
void timer<Clock>::readd_periodic() noexcept {
    arm_state(Clock::now() + _period.value(), {_period.value()});
    engine().queue_timer(this);
}

template <typename Clock>
inline
bool timer<Clock>::cancel() noexcept {
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

template class timer<steady_clock_type>;
template class timer<lowres_clock>;
template class timer<manual_clock>;

#ifdef SEASTAR_BUILD_SHARED_LIBS
thread_local lowres_clock::time_point lowres_clock::_now;
thread_local lowres_system_clock::time_point lowres_system_clock::_now;
#endif

reactor::signals::signals() : _pending_signals(0) {
}

reactor::signals::~signals() {
    sigset_t mask;
    sigfillset(&mask);
    ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
}

reactor::signals::signal_handler::signal_handler(int signo, noncopyable_function<void ()>&& handler)
        : _handler(std::move(handler)) {
}

void
reactor::signals::handle_signal(int signo, noncopyable_function<void ()>&& handler) {
    signal_handler h(signo, std::move(handler));
    auto [_, inserted] =  _signal_handlers.insert_or_assign(signo, std::move(h));
    if (!inserted) {
        // since we register the same handler to OS for all signals, we could
        // skip sigaction when a handler has already been registered before.
        return;
    }

    struct sigaction sa;
    sa.sa_sigaction = [](int sig, siginfo_t *info, void *p) {
        engine()._backend->signal_received(sig, info, p);
    };
    sa.sa_mask = make_empty_sigset_mask();
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    auto r = ::sigaction(signo, &sa, nullptr);
    throw_system_error_on(r == -1);
    auto mask = make_sigset_mask(signo);
    r = ::pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
    throw_pthread_error(r);
}

void
reactor::signals::handle_signal_once(int signo, noncopyable_function<void ()>&& handler) {
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
    engine().start_handling_signal();
    engine()._signals._pending_signals.fetch_or(1ull << signo, std::memory_order_relaxed);
}

void reactor::signals::failed_to_handle(int signo) {
    char tname[64];
    pthread_getname_np(pthread_self(), tname, sizeof(tname));
    auto tid = syscall(SYS_gettid);
    seastar_logger.error("Failed to handle signal {} on thread {} ({}): engine not ready", signo, tid, tname);
}

void reactor::handle_signal(int signo, noncopyable_function<void ()>&& handler) {
    _signals.handle_signal(signo, std::move(handler));
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

    void reserve(size_t len) noexcept {
        SEASTAR_ASSERT(len < _max_size);
        if (_pos + len >= _max_size) {
            flush();
        }
    }

    void append(const char* str, size_t len) noexcept {
        reserve(len);
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
        auto p = convert_hex_safe(buf, sizeof(buf), ptr);
        append(p, (buf + sizeof(buf)) - p);
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

    void append_backtrace_oneline() noexcept {
        backtrace([this] (frame f) noexcept {
            reserve(3 + sizeof(f.addr) * 2);
            append(" 0x");
            append_hex(f.addr);
        });
    }
};

static void print_with_backtrace(backtrace_buffer& buf, bool oneline) noexcept {
    if (local_engine) {
        buf.append(" on shard ");
        buf.append_decimal(this_shard_id());

        buf.append(", in scheduling group ");
        buf.append(current_scheduling_group().name().c_str());
    }

  if (!oneline) {
    buf.append(".\nBacktrace:\n");
    buf.append_backtrace();
  } else {
    buf.append(". Backtrace:");
    buf.append_backtrace_oneline();
    buf.append("\n");
  }
    buf.flush();
}

static void print_with_backtrace(const char* cause, bool oneline = false) noexcept {
    backtrace_buffer buf;
    buf.append(cause);
    print_with_backtrace(buf, oneline);
}

#ifndef SEASTAR_ASAN_ENABLED
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
    return defer([mem = std::move(mem), prev_stack] () mutable noexcept {
        try {
            auto r = sigaltstack(&prev_stack, NULL);
            throw_system_error_on(r == -1);
        } catch (...) {
            mem.release(); // We failed to restore previous stack, must leak it.
            seastar_logger.error("Failed to restore signal stack: {}", std::current_exception());
        }
    });
}
#else
// SIGSTKSZ is too small when using asan. We also don't need to
// handle SIGSEGV ourselves when using asan, so just don't install
// a signal handler stack.
auto install_signal_handler_stack() {
    struct nothing { ~nothing() {} };
    return nothing{};
}
#endif

static sstring shorten_name(const sstring& name, size_t length) {
    SEASTAR_ASSERT(!name.empty());
    SEASTAR_ASSERT(length > 0);

    namespace ba = boost::algorithm;
    using split_iter_t = ba::split_iterator<sstring::const_iterator>;
    static constexpr auto delimiter = "_";

    sstring shortname(typename sstring::initialized_later{}, length);
    auto output = shortname.begin();
    auto last = shortname.end();
    if (name.find(delimiter) == name.npos) {
        // use the prefix as the fallback, if the name is not underscore
        // delimited
        size_t n = std::min(length, name.size());
        output = std::copy_n(name.begin(), n, output);
    } else {
        for (split_iter_t split_it = ba::make_split_iterator(
                 name,
                 ba::token_finder(ba::is_any_of("_"),
                                  ba::token_compress_on)), split_last{};
             output != last && split_it != split_last;
             ++split_it) {
            auto& part = *split_it;
            SEASTAR_ASSERT(part.size() > 0);
            // convert "hello_world" to "hw"
            *output++ = part[0];
        }
    }
    // pad the remaining part with spaces, so the shortened name is always
    // of length long.
    std::fill(output, last, ' ');
    return shortname;
}

reactor::task_queue::task_queue(unsigned id, sstring name, sstring shortname, float shares)
        : _shares(std::max(shares, 1.0f))
        , _reciprocal_shares_times_2_power_32((uint64_t(1) << 32) / _shares)
        , _id(id)
        , _ts(now()) {
    rename(name, shortname);
}

void
reactor::task_queue::register_stats() {
    seastar::metrics::metric_groups new_metrics;
    namespace sm = seastar::metrics;
    static auto group = sm::label("group");
    auto group_label = group(_name);
    new_metrics.add_group("scheduler", {
        sm::make_counter("runtime_ms", [this] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(_runtime).count();
        }, sm::description("Accumulated runtime of this task queue; an increment rate of 1000ms per second indicates full utilization"),
            {group_label}),
        sm::make_counter("waittime_ms", [this] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(_waittime).count();
        }, sm::description("Accumulated waittime of this task queue; an increment rate of 1000ms per second indicates queue is waiting for something (e.g. IO)"),
            {group_label}),
        sm::make_counter("starvetime_ms", [this] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(_starvetime).count();
        }, sm::description("Accumulated starvation time of this task queue; an increment rate of 1000ms per second indicates the scheduler feels really bad"),
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
        sm::make_counter("time_spent_on_task_quota_violations_ms", [this] {
                return _time_spent_on_task_quota_violations / 1ms;
        }, sm::description("Total amount in milliseconds we were in violation of the task quota"),
           {group_label}),
    });

    register_net_metrics_for_scheduling_group(new_metrics, _id, group_label);

    _metrics = std::exchange(new_metrics, {});
}

void
reactor::task_queue::rename(sstring new_name, sstring new_shortname) {
    SEASTAR_ASSERT(!new_name.empty());
    if (_name != new_name) {
        _name = new_name;
        if (new_shortname.empty()) {
            _shortname = shorten_name(_name, shortname_size);
        } else {
            _shortname = fmt::format("{:>{}}", new_shortname, shortname_size);
        }
        register_stats();
    }
}

#ifdef __clang__
__attribute__((no_sanitize("undefined"))) // multiplication below may overflow; we check for that
#elif defined(__GNUC__)
[[gnu::no_sanitize_undefined]]
#endif
inline
int64_t
reactor::task_queue::to_vruntime(sched_clock::duration runtime) const {
    auto scaled = (runtime.count() * _reciprocal_shares_times_2_power_32) >> 32;
    // Prevent overflow from returning ridiculous values
    return std::max<int64_t>(scaled, 0);
}

void
reactor::task_queue::set_shares(float shares) noexcept {
    _shares = std::max(shares, 1.0f);
    _reciprocal_shares_times_2_power_32 = (uint64_t(1) << 32) / _shares;
}

void
reactor::account_runtime(task_queue& tq, sched_clock::duration runtime) {
    if (runtime > (2 * _cfg.task_quota)) {
        _stalls_histogram.add(runtime);
        tq._time_spent_on_task_quota_violations += runtime - _cfg.task_quota;
    }
    tq._vruntime += tq.to_vruntime(runtime);
    tq._runtime += runtime;
}

struct reactor::task_queue::indirect_compare {
    bool operator()(const task_queue* tq1, const task_queue* tq2) const {
        return tq1->_vruntime < tq2->_vruntime;
    }
};

reactor::reactor(std::shared_ptr<smp> smp, alien::instance& alien, unsigned id, reactor_backend_selector rbs, reactor_config cfg)
    : _smp(std::move(smp))
    , _alien(alien)
    , _cfg(std::move(cfg))
    , _notify_eventfd(file_desc::eventfd(0, EFD_CLOEXEC))
    , _task_quota_timer(file_desc::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC))
    , _id(id)
    , _cpu_started(0)
    , _cpu_stall_detector(internal::make_cpu_stall_detector())
    , _reuseport(posix_reuseport_detect())
    , _thread_pool(std::make_unique<thread_pool>(*this, seastar::format("syscall-{}", id))) {
    /*
     * The _backend assignment is here, not on the initialization list as
     * the chosen backend constructor may want to handle signals and thus
     * needs the _signals._signal_handlers map to be initialized.
     */
    _backend = rbs.create(*this);
    *internal::get_scheduling_group_specific_thread_local_data_ptr() = &_scheduling_group_specific_data;
    _task_queues.push_back(std::make_unique<task_queue>(0, "main", "main", 1000));
    _task_queues.push_back(std::make_unique<task_queue>(1, "atexit", "exit", 1000));
    _at_destroy_tasks = _task_queues.back().get();
    set_need_preempt_var(&_preemption_monitor);
    seastar::thread_impl::init();
    _backend->start_tick();

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, internal::cpu_stall_detector::signal_number());
    auto r = ::pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
    SEASTAR_ASSERT(r == 0);
    memory::set_reclaim_hook([this] (std::function<void ()> reclaim_fn) {
        add_high_priority_task(make_task(default_scheduling_group(), [fn = std::move(reclaim_fn)] {
            fn();
        }));
    });
}

reactor::~reactor() {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, internal::cpu_stall_detector::signal_number());
    auto r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
    SEASTAR_ASSERT(r == 0);

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
    for (auto&& tq : _task_queues) {
        if (tq) {
            // The following line will preserve the convention that constructor and destructor functions
            // for the per sg values are called in the context of the containing scheduling group.
            *internal::current_scheduling_group_ptr() = scheduling_group(tq->_id);
            get_sg_data(tq->_id).specific_vals.clear();
        }
    }
}

reactor::sched_stats
reactor::get_sched_stats() const {
    sched_stats ret;
    ret.tasks_processed = tasks_processed();
    return ret;
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

future<> reactor::poll_rdhup(pollable_fd_state& fd) {
    return _backend->poll_rdhup(fd);
}

void reactor::set_strict_dma(bool value) {
    _cfg.strict_o_direct = value;
}

void reactor::set_bypass_fsync(bool value) {
    _cfg.bypass_fsync = value;
}

void
reactor::reset_preemption_monitor() {
    return _backend->reset_preemption_monitor();
}

void
reactor::request_preemption() {
    return _backend->request_preemption();
}

void reactor::start_handling_signal() {
    return _backend->start_handling_signal();
}

namespace internal {

cpu_stall_detector::cpu_stall_detector(cpu_stall_detector_config cfg)
        : _shard_id(this_shard_id()) {
    // glib's backtrace() calls dlopen("libgcc_s.so.1") once to resolve unwind related symbols.
    // If first stall detector invocation happens during another dlopen() call the calling thread
    // will deadlock. The dummy call here makes sure that backtrace's initialization happens in
    // a safe place.
    backtrace([] (frame) {});
    update_config(cfg);

    namespace sm = seastar::metrics;

    _metrics.add_group("stall_detector", {
            sm::make_counter("reported", _total_reported, sm::description("Total number of reported stalls, look in the traces for the exact reason"))});

    // note: if something is added here that can, it should take care to destroy _timer.
}

cpu_stall_detector_posix_timer::cpu_stall_detector_posix_timer(cpu_stall_detector_config cfg) : cpu_stall_detector(cfg) {
    struct sigevent sev = {};
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = signal_number();
#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif
    sev.sigev_notify_thread_id = syscall(SYS_gettid);
    int err = timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &_timer);
    if (err) {
        throw std::system_error(std::error_code(err, std::system_category()));
    }
}

cpu_stall_detector_posix_timer::~cpu_stall_detector_posix_timer() {
    timer_delete(_timer);
}

cpu_stall_detector_config
cpu_stall_detector::get_config() const {
    return _config;
}

void cpu_stall_detector::update_config(cpu_stall_detector_config cfg) {
    _config = cfg;
    _threshold = std::chrono::duration_cast<sched_clock::duration>(cfg.threshold);
    _slack = std::chrono::duration_cast<sched_clock::duration>(cfg.threshold * cfg.slack);
    _max_reports_per_minute = cfg.stall_detector_reports_per_minute;
    _rearm_timer_at = reactor::now();
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
    auto tasks_processed = engine().tasks_processed();
    auto last_seen = _last_tasks_processed_seen.load(std::memory_order_relaxed);
    if (!last_seen) {
        return; // stall detector in not active
    } else if (last_seen == tasks_processed) {
        // we defer to the check of spuriousness to inside this unlikely condition
        // since the check itself can be costly, involving a syscall (reactor::now())
        if (is_spurious_signal()) {
            return;
        }
        // no task was processed - report unless supressed
        maybe_report();
        _report_at <<= 1;
    } else {
        _last_tasks_processed_seen.store(tasks_processed, std::memory_order_relaxed);
    }
    arm_timer();
}

void cpu_stall_detector::report_suppressions(sched_clock::time_point now) {
    if (now > _minute_mark + 60s) {
        if (_reported > _max_reports_per_minute) {
            auto suppressed = _reported - _max_reports_per_minute;
            backtrace_buffer buf;
            // Reuse backtrace buffer infrastructure so we don't have to allocate here
            buf.append("Rate-limit: suppressed ");
            buf.append_decimal(suppressed);
            suppressed == 1 ? buf.append(" backtrace") : buf.append(" backtraces");
            buf.append(" on shard ");
            buf.append_decimal(_shard_id);
            buf.append("\n");
            buf.flush();
        }
        reset_suppression_state(now);
    }
}

void
cpu_stall_detector::reset_suppression_state(sched_clock::time_point now) {
    _reported = 0;
    _minute_mark = now;
}

void cpu_stall_detector_posix_timer::arm_timer() {
    auto its = posix::to_relative_itimerspec(_threshold * _report_at + _slack, 0s);
    timer_settime(_timer, 0, &its, nullptr);
}

void cpu_stall_detector::start_task_run(sched_clock::time_point now) {
    if (now > _rearm_timer_at) {
        report_suppressions(now);
        _report_at = 1;
        _run_started_at = now;
        _rearm_timer_at = now + _threshold * _report_at;
        arm_timer();
    }
    _last_tasks_processed_seen.store(engine().tasks_processed(), std::memory_order_relaxed);
    std::atomic_signal_fence(std::memory_order_release); // Don't delay this write, so the signal handler can see it
}

void cpu_stall_detector::end_task_run(sched_clock::time_point now) {
    std::atomic_signal_fence(std::memory_order_acquire); // Don't hoist this write, so the signal handler can see it
    _last_tasks_processed_seen.store(0, std::memory_order_relaxed);
}

void cpu_stall_detector_posix_timer::start_sleep() {
    auto its = posix::to_relative_itimerspec(0s,  0s);
    timer_settime(_timer, 0, &its, nullptr);
    _rearm_timer_at = reactor::now();
}

void cpu_stall_detector::end_sleep() {
}

static long
perf_event_open(struct perf_event_attr* hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

cpu_stall_detector_linux_perf_event::cpu_stall_detector_linux_perf_event(file_desc fd, cpu_stall_detector_config cfg)
        : cpu_stall_detector(cfg), _fd(std::move(fd)) {
    void* ret = ::mmap(nullptr, 2*getpagesize(), PROT_READ|PROT_WRITE, MAP_SHARED, _fd.get(), 0);
    if (ret == MAP_FAILED) {
        abort();
    }
    _mmap = static_cast<struct ::perf_event_mmap_page*>(ret);
    _data_area = reinterpret_cast<char*>(_mmap) + getpagesize();
    _data_area_mask = getpagesize() - 1;
}

cpu_stall_detector_linux_perf_event::~cpu_stall_detector_linux_perf_event() {
    ::munmap(_mmap, 2*getpagesize());
}

void
cpu_stall_detector_linux_perf_event::arm_timer() {
    auto period = _threshold * _report_at + _slack;
    uint64_t ns =  period / 1ns;
    _next_signal_time = reactor::now() + period;

    // clear out any existing records in the ring buffer, so when we get interrupted next time
    // we have only the stack associated with that interrupt, and so we don't overflow.
    data_area_reader(*this).skip_all();
    if (__builtin_expect(_enabled && _current_period == ns, 1)) {
        // Common case - we're re-arming with the same period, the counter
        // is already enabled.

        // We want to set the next interrupt to ns from now, and somewhat oddly the
        // way to do this is PERF_EVENT_IOC_PERIOD, even with the same period as
        // already configured, see the code at:
        //
        // https://elixir.bootlin.com/linux/v5.15.86/source/kernel/events/core.c#L5636
        //
        // Ths change is intentional: kernel commit bad7192b842c83e580747ca57104dd51fe08c223
        // so we can resumably rely on it.
        _fd.ioctl(PERF_EVENT_IOC_PERIOD, ns);

    } else {
        // Uncommon case - we're moving from disabled to enabled, or changing
        // the period. Issue more calls and be careful.
        _fd.ioctl(PERF_EVENT_IOC_DISABLE, 0); // avoid false alarms while we modify stuff
        _fd.ioctl(PERF_EVENT_IOC_PERIOD, ns);
        _fd.ioctl(PERF_EVENT_IOC_RESET, 0);
        _fd.ioctl(PERF_EVENT_IOC_ENABLE, 0);
        _enabled = true;
        _current_period = ns;
    }
}

void
cpu_stall_detector_linux_perf_event::start_sleep() {
    _fd.ioctl(PERF_EVENT_IOC_DISABLE, 0);
    _enabled = false;
}

bool
cpu_stall_detector_linux_perf_event::is_spurious_signal() {
    // If the current time is before the expected signal time, it is
    // probably a spurious signal. One reason this could occur is that
    // PERF_EVENT_IOC_PERIOD does not reset the current overflow point
    // on kernels prior to 3.14 (or 3.7 on Arm).
    return reactor::now() < _next_signal_time;
}

void
cpu_stall_detector_linux_perf_event::maybe_report_kernel_trace() {
    data_area_reader reader(*this);
    auto current_record = [&] () -> ::perf_event_header {
        return reader.read_struct<perf_event_header>();
    };

    while (reader.have_data()) {
        auto record = current_record();

        if (record.type != PERF_RECORD_SAMPLE) {
            reader.skip(record.size - sizeof(record));
            continue;
        }

        auto nr = reader.read_u64();
        backtrace_buffer buf;
        buf.append("kernel callstack:");
        for (uint64_t i = 0; i < nr; ++i) {
            buf.append(" 0x");
            buf.append_hex(uintptr_t(reader.read_u64()));
        }
        buf.append("\n");
        buf.flush();
    };
}

std::unique_ptr<cpu_stall_detector_linux_perf_event>
cpu_stall_detector_linux_perf_event::try_make(cpu_stall_detector_config cfg) {
    ::perf_event_attr pea = {
        .type = PERF_TYPE_SOFTWARE,
        .size = sizeof(pea),
        .config = PERF_COUNT_SW_TASK_CLOCK, // more likely to work on virtual machines than hardware events
        .sample_period = 1'000'000'000, // Needs non-zero value or PERF_IOC_PERIOD gets confused
        .sample_type = PERF_SAMPLE_CALLCHAIN,
        .disabled = 1,
        .exclude_callchain_user = 1,  // we're using backtrace() to capture the user callchain
        .wakeup_events = 1,
    };
    unsigned long flags = 0;
    if (internal::kernel_uname().whitelisted({"3.14"})) {
        flags |= PERF_FLAG_FD_CLOEXEC;
    }
    int fd = perf_event_open(&pea, 0, -1, -1, flags);
    if (fd == -1) {
        throw std::system_error(errno, std::system_category(), "perf_event_open() failed");
    }
    auto desc = file_desc::from_fd(fd);
    struct f_owner_ex sig_owner = {
        .type = F_OWNER_TID,
        .pid = static_cast<pid_t>(syscall(SYS_gettid)),
    };
    auto ret1 = ::fcntl(fd, F_SETOWN_EX, &sig_owner);
    if (ret1 == -1) {
        abort();
    }
    auto ret2 = ::fcntl(fd, F_SETSIG, signal_number());
    if (ret2 == -1) {
        abort();
    }
    auto fd_flags = ::fcntl(fd, F_GETFL);
    if (fd_flags == -1) {
        abort();
    }
    auto ret3 = ::fcntl(fd, F_SETFL, fd_flags | O_ASYNC);
    if (ret3 == -1) {
        abort();
    }
    return std::make_unique<cpu_stall_detector_linux_perf_event>(std::move(desc), std::move(cfg));
}


std::unique_ptr<cpu_stall_detector> make_cpu_stall_detector(cpu_stall_detector_config cfg) {
    bool was_eaccess_failure = false;
    try {
        try {
            return cpu_stall_detector_linux_perf_event::try_make(cfg);
        } catch (std::system_error& e) {
            // This failure occurs when /proc/sys/kernel/perf_event_paranoid is set
            // to 2 or higher, and is expected since most distributions set it to that
            // way as of 2023. In this case we log a different message and only at INFO
            // level on shard 0.
            was_eaccess_failure = e.code() == std::error_code(EACCES, std::system_category());
            throw;
        }
    } catch (...) {
        if (was_eaccess_failure) {
            seastar_logger.info0("Perf-based stall detector creation failed (EACCESS), "
                    "try setting /proc/sys/kernel/perf_event_paranoid to 1 or less to "
                    "enable kernel backtraces: falling back to posix timer.");
        } else {
            seastar_logger.warn("Creation of perf_event based stall detector failed: falling back to posix timer: {}", std::current_exception());
        }
        return std::make_unique<cpu_stall_detector_posix_timer>(cfg);
    }
}

void cpu_stall_detector::generate_trace() {
    auto delta = reactor::now() - _run_started_at;

    _total_reported++;
    if (_config.report) {
        _config.report();
        return;
    }

    backtrace_buffer buf;
    buf.append("Reactor stalled for ");
    buf.append_decimal(uint64_t(delta / 1ms));
    buf.append(" ms");
    print_with_backtrace(buf, _config.oneline);
    maybe_report_kernel_trace();
}

} // internal namespace

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
reactor::test::set_stall_detector_report_function(std::function<void ()> report) {
    auto& r = engine();
    auto cfg = r._cpu_stall_detector->get_config();
    cfg.report = std::move(report);
    r._cpu_stall_detector->update_config(std::move(cfg));
    r._cpu_stall_detector->reset_suppression_state(reactor::now());
}

std::function<void ()>
reactor::test::get_stall_detector_report_function() {
    return engine()._cpu_stall_detector->get_config().report;
}

void
reactor::block_notifier(int) {
    engine()._cpu_stall_detector->on_signal();
}

class network_stack_factory {
    network_stack_entry::factory_func _func;

public:
    network_stack_factory(noncopyable_function<future<std::unique_ptr<network_stack>> (const program_options::option_group&)> func)
        : _func(std::move(func)) { }
    future<std::unique_ptr<network_stack>> operator()(const program_options::option_group& opts) { return _func(opts); }
};

void reactor::configure(const reactor_options& opts) {
    _network_stack_ready = opts.network_stack.get_selected_candidate()(*opts.network_stack.get_selected_candidate_opts());

    auto blocked_time = opts.blocked_reactor_notify_ms.get_value() * 1ms;
    internal::cpu_stall_detector_config csdc;
    csdc.threshold = blocked_time;
    csdc.stall_detector_reports_per_minute = opts.blocked_reactor_reports_per_minute.get_value();
    csdc.oneline = opts.blocked_reactor_report_format_oneline.get_value();
    _cpu_stall_detector->update_config(csdc);

    if (_cfg.no_poll_aio) {
        _aio_eventfd = pollable_fd(file_desc::eventfd(0, 0));
    }
}

pollable_fd
reactor::posix_listen(socket_address sa, listen_options opts) {
    auto specific_protocol = (int)(opts.proto);
    if (sa.is_af_unix()) {
        // no type-safe way to create listen_opts with proto=0
        specific_protocol = 0;
    }
    static auto somaxconn = [] {
        std::optional<int> result;
        std::ifstream ifs("/proc/sys/net/core/somaxconn");
        if (ifs) {
            result = 0;
            ifs >> *result;
        }
        return result;
    }();
    if (somaxconn && *somaxconn < opts.listen_backlog) {
        fmt::print(
            "Warning: /proc/sys/net/core/somaxconn is set to {:d} "
            "which is lower than the backlog parameter {:d} used for listen(), "
            "please change it with `sysctl -w net.core.somaxconn={:d}`\n",
            *somaxconn, opts.listen_backlog, opts.listen_backlog);
    }

    file_desc fd = file_desc::socket(sa.u.sa.sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, specific_protocol);
    if (opts.reuse_address) {
        fd.setsockopt(SOL_SOCKET, SO_REUSEADDR, 1);
    }

    if (opts.so_sndbuf) {
        fd.setsockopt(SOL_SOCKET, SO_SNDBUF, *opts.so_sndbuf);
    }

    if (opts.so_rcvbuf) {
        fd.setsockopt(SOL_SOCKET, SO_RCVBUF, *opts.so_rcvbuf);
    }

    if (_reuseport && !sa.is_af_unix())
        fd.setsockopt(SOL_SOCKET, SO_REUSEPORT, 1);

    try {
        fd.bind(sa.u.sa, sa.length());

        if (sa.is_af_unix() && opts.unix_domain_socket_permissions) {
            // After bind the socket is created in the file system.
            mode_t mode = static_cast<mode_t>(opts.unix_domain_socket_permissions.value());
            auto result = ::chmod(sa.u.un.sun_path, mode);
            if (result < 0) {
                auto errno_copy = errno;
                ::unlink(sa.u.un.sun_path);
                throw std::system_error(errno_copy, std::system_category(), "chmod failed");
            }
        }

        fd.listen(opts.listen_backlog);
    } catch (const std::system_error& s) {
        throw std::system_error(s.code(), fmt::format("posix_listen failed for address {}", sa));
    }

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

void pollable_fd_state::maybe_no_more_recv() {
    if (shutdown_mask & posix::rcv_shutdown) {
        throw std::system_error(std::error_code(ECONNABORTED, std::system_category()));
    }
}

void pollable_fd_state::maybe_no_more_send() {
    if (shutdown_mask & posix::snd_shutdown) {
        throw std::system_error(std::error_code(ECONNABORTED, std::system_category()));
    }
}

void pollable_fd_state::forget() {
    engine()._backend->forget(*this);
}

void intrusive_ptr_release(pollable_fd_state* fd) {
    if (!--fd->_refs) {
        fd->forget();
    }
}

pollable_fd::pollable_fd(file_desc fd, pollable_fd::speculation speculate)
    : _s(engine()._backend->make_pollable_fd_state(std::move(fd), speculate))
{}

void pollable_fd::shutdown(int how, shutdown_kernel_only kernel_only) {
    if (!kernel_only) {
        // TCP will respond to shutdown() by returning ECONNABORT on the next IO,
        // but UDP responds by returning AGAIN. The shutdown_mask tells us to convert
        // EAGAIN to ECONNABORT in that case.
        _s->shutdown_mask |= posix::shutdown_mask(how);
    }
    engine()._backend->shutdown(*_s, how);
}

pollable_fd
reactor::make_pollable_fd(socket_address sa, int proto) {
    int maybe_nonblock = _backend->do_blocking_io() ? 0 : SOCK_NONBLOCK;
    file_desc fd = file_desc::socket(sa.u.sa.sa_family, SOCK_STREAM | maybe_nonblock | SOCK_CLOEXEC, proto);
    return pollable_fd(std::move(fd));
}

future<>
reactor::posix_connect(pollable_fd pfd, socket_address sa, socket_address local) {
#ifdef IP_BIND_ADDRESS_NO_PORT
    if (!sa.is_af_unix()) {
        try {
            // do not reserve an ephemeral port when using bind() with port number 0.
            // connect() will handle it later. The reason for that is that bind() may fail
            // to allocate a port while connect will success, this is because bind() does not
            // know dst address and has to find globally unique local port.
            pfd.get_file_desc().setsockopt(SOL_IP, IP_BIND_ADDRESS_NO_PORT, 1);
        } catch (std::system_error& err) {
            if (err.code() !=  std::error_code(ENOPROTOOPT, std::system_category())) {
                throw;
            }
        }
    }
#endif
    if (!local.is_wildcard()) {
        // call bind() only if local address is not wildcard
        pfd.get_file_desc().bind(local.u.sa, local.length());
    }
    return pfd.connect(sa).finally([pfd] {});
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

void io_completion::complete_with(ssize_t res) {
    if (res >= 0) {
        complete(res);
        return;
    }

    ++engine()._io_stats.aio_errors;
    try {
        throw_kernel_error(res);
    } catch (...) {
        set_exception(std::current_exception());
    }
}

bool
reactor::flush_pending_aio() {
    for (auto& ioq : _io_queues) {
        ioq.second->poll_io_queue();
    }
    return false;
}

steady_clock_type::time_point reactor::next_pending_aio() const noexcept {
    steady_clock_type::time_point next = steady_clock_type::time_point::max();

    for (auto& ioq : _io_queues) {
        steady_clock_type::time_point n = ioq.second->next_pending_aio();
        if (n < next) {
            next = std::move(n);
        }
    }

    return next;
}

bool
reactor::reap_kernel_completions() {
    return _backend->reap_kernel_completions();
}

namespace internal {

size_t sanitize_iovecs(std::vector<iovec>& iov, size_t disk_alignment) noexcept {
    if (iov.size() > IOV_MAX) {
        iov.resize(IOV_MAX);
    }
    auto length = iovec_len(iov);
    while (auto rest = length & (disk_alignment - 1)) {
        if (iov.back().iov_len <= rest) {
            length -= iov.back().iov_len;
            iov.pop_back();
        } else {
            iov.back().iov_len -= rest;
            length -= rest;
        }
    }
    return length;
}

}

future<file>
reactor::open_file_dma(std::string_view nameref, open_flags flags, file_open_options options) noexcept {
    return do_with(static_cast<int>(flags), std::move(options), [this, nameref] (auto& open_flags, file_open_options& options) {
        sstring name(nameref);
        return _thread_pool->submit<syscall_result_extra<struct stat>>([this, name, &open_flags, &options, strict_o_direct = _cfg.strict_o_direct, bypass_fsync = _cfg.bypass_fsync] () mutable {
            // We want O_DIRECT, except in three cases:
            //   - tmpfs (which doesn't support it, but works fine anyway)
            //   - strict_o_direct == false (where we forgive it being not supported)
            //   - kernel_page_cache == true (where we disable it for short-lived test processes)
            // Because open() with O_DIRECT will fail, we open it without O_DIRECT, try
            // to update it to O_DIRECT with fcntl(), and if that fails, see if we
            // can forgive it.
            auto is_tmpfs = [] (int fd) {
                struct ::statfs buf;
                auto r = ::fstatfs(fd, &buf);
                if (r == -1) {
                    return false;
                }
                return buf.f_type == internal::fs_magic::tmpfs;
            };
            open_flags |= O_CLOEXEC;
            if (bypass_fsync) {
                open_flags &= ~O_DSYNC;
            }
            struct stat st;
            auto mode = static_cast<mode_t>(options.create_permissions);
            int fd = ::open(name.c_str(), open_flags, mode);
            if (fd == -1) {
                return wrap_syscall(fd, st);
            }
            auto close_fd = defer([fd] () noexcept { ::close(fd); });
            int o_direct_flag = _cfg.kernel_page_cache ? 0 : O_DIRECT;
            int r = ::fcntl(fd, F_SETFL, open_flags | o_direct_flag);
            if (r == -1  && strict_o_direct) {
                auto maybe_ret = wrap_syscall(r, st);  // capture errno (should be EINVAL)
                if (!is_tmpfs(fd)) {
                    return maybe_ret;
                }
            }
            if (fd != -1 && options.extent_allocation_size_hint && !_cfg.kernel_page_cache) {
                fsxattr attr = {};
                int r = ::ioctl(fd, XFS_IOC_FSGETXATTR, &attr);
                // xfs delayed allocation is disabled when extent size hints are present.
                // This causes tons of xfs log fsyncs. Given that extent size hints are
                // unneeded when delayed allocation is available (which is the case
                // when not using O_DIRECT), disable them.
                //
                // Ignore error; may be !xfs, and just a hint anyway
                if (r != -1) {
                    attr.fsx_xflags |= XFS_XFLAG_EXTSIZE;
                    attr.fsx_extsize = std::min(options.extent_allocation_size_hint,
                                        file_open_options::max_extent_allocation_size_hint);

                    attr.fsx_extsize = align_up<uint32_t>(attr.fsx_extsize, file_open_options::min_extent_size_hint_alignment);

                    // Ignore error; may be !xfs, and just a hint anyway
                    ::ioctl(fd, XFS_IOC_FSSETXATTR, &attr);
                }
            }
            r = ::fstat(fd, &st);
            if (r == -1) {
                return wrap_syscall(r, st);
            }
            close_fd.cancel();
            return wrap_syscall(fd, st);
        }).then([&options, name = std::move(name), &open_flags] (syscall_result_extra<struct stat> sr) {
            sr.throw_fs_exception_if_error("open failed", name);
            return make_file_impl(sr.result, options, open_flags, sr.extra);
        }).then([] (shared_ptr<file_impl> impl) {
            return make_ready_future<file>(std::move(impl));
        });
    });
}

future<>
reactor::remove_file(std::string_view pathname) noexcept {
    // Allocating memory for a sstring can throw, hence the futurize_invoke
    return futurize_invoke([this, pathname] {
        return _thread_pool->submit<syscall_result<int>>([pathname = sstring(pathname)] {
            return wrap_syscall<int>(::remove(pathname.c_str()));
        }).then([pathname = sstring(pathname)] (syscall_result<int> sr) {
            sr.throw_fs_exception_if_error("remove failed", pathname);
            return make_ready_future<>();
        });
    });
}

future<>
reactor::rename_file(std::string_view old_pathname, std::string_view new_pathname) noexcept {
    // Allocating memory for a sstring can throw, hence the futurize_invoke
    return futurize_invoke([this, old_pathname, new_pathname] {
        return _thread_pool->submit<syscall_result<int>>([old_pathname = sstring(old_pathname), new_pathname = sstring(new_pathname)] {
            return wrap_syscall<int>(::rename(old_pathname.c_str(), new_pathname.c_str()));
        }).then([old_pathname = sstring(old_pathname), new_pathname = sstring(new_pathname)] (syscall_result<int> sr) {
            sr.throw_fs_exception_if_error("rename failed",  old_pathname, new_pathname);
            return make_ready_future<>();
        });
    });
}

future<>
reactor::link_file(std::string_view oldpath, std::string_view newpath) noexcept {
    // Allocating memory for a sstring can throw, hence the futurize_invoke
    return futurize_invoke([this, oldpath, newpath] {
        return _thread_pool->submit<syscall_result<int>>([oldpath = sstring(oldpath), newpath = sstring(newpath)] {
            return wrap_syscall<int>(::link(oldpath.c_str(), newpath.c_str()));
        }).then([oldpath = sstring(oldpath), newpath = sstring(newpath)] (syscall_result<int> sr) {
            sr.throw_fs_exception_if_error("link failed", oldpath, newpath);
            return make_ready_future<>();
        });
    });
}

future<>
reactor::chmod(std::string_view name, file_permissions permissions) noexcept {
    auto mode = static_cast<mode_t>(permissions);
    // Allocating memory for a sstring can throw, hence the futurize_invoke
    return futurize_invoke([name, mode, this] {
        return _thread_pool->submit<syscall_result<int>>([name = sstring(name), mode] {
            return wrap_syscall<int>(::chmod(name.c_str(), mode));
        }).then([name = sstring(name), mode] (syscall_result<int> sr) {
            if (sr.result == -1) {
                auto reason = format("chmod(0{:o}) failed", mode);
                sr.throw_fs_exception(reason, fs::path(name));
            }
            return make_ready_future<>();
        });
    });
}

directory_entry_type stat_to_entry_type(mode_t type) {
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
    if (S_ISSOCK(type)) {
        return directory_entry_type::socket;
    }
    if (S_ISREG(type)) {
        return directory_entry_type::regular;
    }
    return directory_entry_type::unknown;
}

future<std::optional<directory_entry_type>>
reactor::file_type(std::string_view name, follow_symlink follow) noexcept {
    // Allocating memory for a sstring can throw, hence the futurize_invoke
    return futurize_invoke([name, follow, this] {
        return _thread_pool->submit<syscall_result_extra<struct stat>>([name = sstring(name), follow] {
            struct stat st;
            auto stat_syscall = follow ? stat : lstat;
            auto ret = stat_syscall(name.c_str(), &st);
            return wrap_syscall(ret, st);
        }).then([name = sstring(name)] (syscall_result_extra<struct stat> sr) {
            if (long(sr.result) == -1) {
                if (sr.error != ENOENT && sr.error != ENOTDIR) {
                    sr.throw_fs_exception_if_error("stat failed", name);
                }
                return make_ready_future<std::optional<directory_entry_type> >
                    (std::optional<directory_entry_type>() );
            }
            return make_ready_future<std::optional<directory_entry_type> >
                (std::optional<directory_entry_type>(stat_to_entry_type(sr.extra.st_mode)) );
        });
    });
}

future<std::optional<directory_entry_type>>
file_type(std::string_view name, follow_symlink follow) noexcept {
    return engine().file_type(name, follow);
}

static std::chrono::system_clock::time_point
timespec_to_time_point(const timespec& ts) {
    auto d = std::chrono::duration_cast<std::chrono::system_clock::duration>(
            ts.tv_sec * 1s + ts.tv_nsec * 1ns);
    return std::chrono::system_clock::time_point(d);
}

future<size_t> reactor::read_directory(int fd, char* buffer, size_t buffer_size) {
    return _thread_pool->submit<syscall_result<long>>([fd, buffer, buffer_size] () {
        auto ret = ::syscall(__NR_getdents64, fd, reinterpret_cast<linux_dirent64*>(buffer), buffer_size);
        return wrap_syscall(ret);
    }).then([] (syscall_result<long> ret) {
        ret.throw_if_error();
        return make_ready_future<size_t>(ret.result);
    });
}

future<int>
reactor::inotify_add_watch(int fd, std::string_view path, uint32_t flags) {
    // Allocating memory for a sstring can throw, hence the futurize_invoke
    return futurize_invoke([path, fd, flags, this] {
        return _thread_pool->submit<syscall_result<int>>([fd, path = sstring(path), flags] {
            auto ret = ::inotify_add_watch(fd, path.c_str(), flags);
            return wrap_syscall(ret);
        }).then([] (syscall_result<int> ret) {
            ret.throw_if_error();
            return make_ready_future<int>(ret.result);
        });
    });
}

future<std::tuple<file_desc, file_desc>>
reactor::make_pipe() {
    return do_with(std::array<int, 2>{}, [this] (auto& pipe) {
        return _thread_pool->submit<syscall_result<int>>([&pipe] {
            return wrap_syscall<int>(::pipe2(pipe.data(), O_NONBLOCK));
        }).then([&pipe] (syscall_result<int> ret) {
            ret.throw_if_error();
            return make_ready_future<std::tuple<file_desc, file_desc>>(file_desc::from_fd(pipe[0]),
                                                                       file_desc::from_fd(pipe[1]));
        });
    });
}

future<std::tuple<pid_t, file_desc, file_desc, file_desc>>
reactor::spawn(std::string_view pathname,
               std::vector<sstring> argv,
               std::vector<sstring> env) {
    return when_all_succeed(make_pipe(),
                            make_pipe(),
                            make_pipe()).then_unpack([pathname = sstring(pathname),
                                                      argv = std::move(argv),
                                                      env = std::move(env), this] (std::tuple<file_desc, file_desc> cin_pipe,
                                                                                   std::tuple<file_desc, file_desc> cout_pipe,
                                                                                   std::tuple<file_desc, file_desc> cerr_pipe) mutable {
        return do_with(pid_t{},
                       std::move(cin_pipe),
                       std::move(cout_pipe),
                       std::move(cerr_pipe),
                       std::move(pathname),
                       posix_spawn_file_actions_t{},
                       posix_spawnattr_t{},
                       std::move(argv),
                       std::move(env),
                       [this](auto& child_pid, auto& cin_pipe, auto& cout_pipe, auto& cerr_pipe, auto& pathname, auto& actions, auto& attr, auto& argv, auto& env) {
            static constexpr int pipefd_read_end = 0;
            static constexpr int pipefd_write_end = 1;
            // Allocating memory for spawn {file actions,attributes} objects can throw, hence the futurize_invoke
            return futurize_invoke([&child_pid, &cin_pipe, &cout_pipe, &cerr_pipe, &pathname, &actions, &attr, &argv, &env, this] {
                // the args and envs parameters passed to posix_spawn() should be array of pointers, and
                // the last one should be a null pointer.
                std::vector<const char*> argvp;
                std::transform(argv.cbegin(), argv.cend(), std::back_inserter(argvp),
                               [](auto& s) { return s.c_str(); });
                argvp.push_back(nullptr);

                std::vector<const char*> envp;
                std::transform(env.cbegin(), env.cend(), std::back_inserter(envp),
                               [](auto& s) { return s.c_str(); });
                envp.push_back(nullptr);

                int r = 0;
                r = ::posix_spawn_file_actions_init(&actions);
                throw_pthread_error(r);
                // the child process does not write to stdin
                std::get<pipefd_write_end>(cin_pipe).spawn_actions_add_close(&actions);
                // the child process does not read from stdout
                std::get<pipefd_read_end>(cout_pipe).spawn_actions_add_close(&actions);
                // the child process does not read from stderr
                std::get<pipefd_read_end>(cerr_pipe).spawn_actions_add_close(&actions);
                // redirect stdin, stdout and stderr to cin_pipe, cout_pipe and cerr_pipe respectively
                std::get<pipefd_read_end>(cin_pipe).spawn_actions_add_dup2(&actions, STDIN_FILENO);
                std::get<pipefd_write_end>(cout_pipe).spawn_actions_add_dup2(&actions, STDOUT_FILENO);
                std::get<pipefd_write_end>(cerr_pipe).spawn_actions_add_dup2(&actions, STDERR_FILENO);
                // after dup2() the interesting ends of pipes, close them
                std::get<pipefd_read_end>(cin_pipe).spawn_actions_add_close(&actions);
                std::get<pipefd_write_end>(cout_pipe).spawn_actions_add_close(&actions);
                std::get<pipefd_write_end>(cerr_pipe).spawn_actions_add_close(&actions);
                // tools like "cat" expect a fd opened in blocking mode when performing I/O
                std::get<pipefd_read_end>(cin_pipe).template ioctl<int>(FIONBIO, 0);
                std::get<pipefd_write_end>(cout_pipe).template ioctl<int>(FIONBIO, 0);
                std::get<pipefd_write_end>(cerr_pipe).template ioctl<int>(FIONBIO, 0);

                r = ::posix_spawnattr_init(&attr);
                throw_pthread_error(r);
                // make sure the following signals are not ignored by the child process
                sigset_t default_signals;
                sigemptyset(&default_signals);
                sigaddset(&default_signals, SIGINT);
                sigaddset(&default_signals, SIGTERM);
                r = ::posix_spawnattr_setsigdefault(&attr, &default_signals);
                throw_pthread_error(r);
                // make sure no signals are marked in the child process
                sigset_t mask_signals;
                sigemptyset(&mask_signals);
                r = ::posix_spawnattr_setsigmask(&attr, &mask_signals);
                throw_pthread_error(r);
                r = ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
                throw_pthread_error(r);

                return _thread_pool->submit<syscall_result<int>>([&child_pid, &pathname, &actions, &attr,
                                                                  argv = std::move(argvp),
                                                                  env =  std::move(envp)] {
                    return wrap_syscall<int>(::posix_spawn(&child_pid, pathname.c_str(), &actions, &attr,
                                                           const_cast<char* const *>(argv.data()),
                                                           const_cast<char* const *>(env.data())));
            });
        }).finally([&actions, &attr] {
            posix_spawn_file_actions_destroy(&actions);
            posix_spawnattr_destroy(&attr);
        }).then([&child_pid, &cin_pipe, &cout_pipe, &cerr_pipe] (syscall_result<int> ret) {
            throw_pthread_error(ret.result);
            return make_ready_future<std::tuple<pid_t, file_desc, file_desc, file_desc>>(
                    child_pid,
                    std::get<pipefd_write_end>(std::move(cin_pipe)),
                    std::get<pipefd_read_end>(std::move(cout_pipe)),
                    std::get<pipefd_read_end>(std::move(cerr_pipe)));
            });
        });
    });
}

static auto next_waitpid_timeout(std::chrono::milliseconds this_timeout) {
    constexpr std::chrono::milliseconds step_timeout(20);
    constexpr std::chrono::milliseconds max_timeout(1000);
    if (this_timeout >= max_timeout) {
        return max_timeout;
    }
    return this_timeout + step_timeout;
}

#ifndef __NR_pidfd_open

#  if defined(__alpha__)
#    define __NR_pidfd_open 544
#  else
#    define __NR_pidfd_open 434
#  endif

#endif

future<int> reactor::waitpid(pid_t pid) {
    syscall_result<int> pidfd = co_await _thread_pool->submit<syscall_result<int>>([pid] {
        return wrap_syscall<int>(syscall(__NR_pidfd_open, pid, O_NONBLOCK));
    });
    // pidfd_open() was introduced in linux 5.3, so the pidfd.error could be ENOSYS on
    // older kernels. But it could be other error like EMFILE or ENFILE. anyway, we
    // should always waitpid().
    std::optional<pollable_fd> pfd;
    if (pidfd.result != -1) {
        pfd.emplace(file_desc::from_fd(pidfd.result));
        co_await pfd->readable();
    }

    auto do_waitpid = [this] (pid_t pid) -> future<std::optional<int>> {
        int wstatus;
        auto ret = co_await _thread_pool->submit<syscall_result<pid_t>>([&] {
            return wrap_syscall<pid_t>(::waitpid(pid, &wstatus, WNOHANG));
        });
        if (ret.result == 0) {
            // Result not ready yet (with WNOHANG)
            co_return std::nullopt;
        } else if (ret.result > 0) {
            // Success.  Return the waited pid status
            co_return wstatus;
        } else {
            // Error.  Maybe throw exception, or return -1 status.
            ret.throw_if_error();
            co_return -1;
        }
    };

    std::optional<int> ret_opt;
    std::chrono::milliseconds wait_timeout(0);
    while (!(ret_opt = co_await do_waitpid(pid))) {
        wait_timeout = next_waitpid_timeout(wait_timeout);
        co_await sleep(wait_timeout);
    }
    co_return *ret_opt;
}

void reactor::kill(pid_t pid, int sig) {
    auto ret = wrap_syscall<int>(::kill(pid, sig));
    ret.throw_if_error();
}

future<std::optional<struct group_details>> reactor::getgrnam(std::string_view name) {
    syscall_result_extra<std::optional<struct group_details>> sr = co_await _thread_pool->submit<syscall_result_extra<std::optional<struct group_details>>>(
        [name = sstring(name)] {
            struct group grp;
            struct group *result;
            memset(&grp, 0, sizeof(struct group));
            char buf[1024];
            errno = 0;
            int ret = ::getgrnam_r(name.c_str(), &grp, buf, sizeof(buf), &result);
            if (!result) {
                return wrap_syscall(ret, std::optional<struct group_details>(std::nullopt));
            }

            group_details gd;
            gd.group_name = sstring(grp.gr_name);
            gd.group_passwd = sstring(grp.gr_passwd);
            gd.group_id = grp.gr_gid;
            for (char **members = grp.gr_mem; *members != nullptr; ++members) {
                gd.group_members.emplace_back(sstring(*members));
            }
            return wrap_syscall(ret, std::optional<struct group_details>(gd));
        });

    if (sr.result != 0) {
        throw std::system_error(sr.ec());
    }

    co_return sr.extra;
}

future<> reactor::chown(std::string_view filepath, uid_t owner, gid_t group) {
    syscall_result<int> sr = co_await _thread_pool->submit<syscall_result<int>>(
        [filepath = sstring(filepath), owner, group] {
            int ret = ::chown(filepath.c_str(), owner, group);
            return wrap_syscall(ret);
        });

    sr.throw_if_error();
    co_return;
}

future<stat_data>
reactor::file_stat(std::string_view pathname, follow_symlink follow) noexcept {
    // Allocating memory for a sstring can throw, hence the futurize_invoke
    return futurize_invoke([pathname, follow, this] {
        return _thread_pool->submit<syscall_result_extra<struct stat>>([pathname = sstring(pathname), follow] {
            struct stat st;
            auto stat_syscall = follow ? stat : lstat;
            auto ret = stat_syscall(pathname.c_str(), &st);
            return wrap_syscall(ret, st);
        }).then([pathname = sstring(pathname)] (syscall_result_extra<struct stat> sr) {
            sr.throw_fs_exception_if_error("stat failed", pathname);
            struct stat& st = sr.extra;
            stat_data sd;
            sd.device_id = st.st_dev;
            sd.inode_number = st.st_ino;
            sd.mode = st.st_mode;
            sd.type = stat_to_entry_type(st.st_mode);
            sd.number_of_links = st.st_nlink;
            sd.uid = st.st_uid;
            sd.gid = st.st_gid;
            sd.rdev = st.st_rdev;
            sd.size = st.st_size;
            sd.block_size = st.st_blksize;
            sd.allocated_size = st.st_blocks * 512UL;
            sd.time_accessed = timespec_to_time_point(st.st_atim);
            sd.time_modified = timespec_to_time_point(st.st_mtim);
            sd.time_changed = timespec_to_time_point(st.st_ctim);
            return make_ready_future<stat_data>(std::move(sd));
        });
    });
}

future<uint64_t>
reactor::file_size(std::string_view pathname) noexcept {
    return file_stat(pathname, follow_symlink::yes).then([] (stat_data sd) {
        return make_ready_future<uint64_t>(sd.size);
    });
}

future<bool>
reactor::file_accessible(std::string_view pathname, access_flags flags) noexcept {
    // Allocating memory for a sstring can throw, hence the futurize_invoke
    return futurize_invoke([pathname, flags, this] {
        return _thread_pool->submit<syscall_result<int>>([pathname = sstring(pathname), flags] {
            auto aflags = std::underlying_type_t<access_flags>(flags);
            auto ret = ::access(pathname.c_str(), aflags);
            return wrap_syscall(ret);
        }).then([pathname = sstring(pathname), flags] (syscall_result<int> sr) {
            if (sr.result < 0) {
                if ((sr.error == ENOENT && flags == access_flags::exists) ||
                    (sr.error == EACCES && flags != access_flags::exists)) {
                    return make_ready_future<bool>(false);
                }
                sr.throw_fs_exception("access failed", fs::path(pathname));
            }

            return make_ready_future<bool>(true);
        });
    });
}

future<fs_type>
reactor::file_system_at(std::string_view pathname) noexcept {
    // Allocating memory for a sstring can throw, hence the futurize_invoke
    return futurize_invoke([pathname, this] {
        return _thread_pool->submit<syscall_result_extra<struct statfs>>([pathname = sstring(pathname)] {
            struct statfs st;
            auto ret = statfs(pathname.c_str(), &st);
            return wrap_syscall(ret, st);
        }).then([pathname = sstring(pathname)] (syscall_result_extra<struct statfs> sr) {
            static std::unordered_map<long int, fs_type> type_mapper = {
                { internal::fs_magic::xfs, fs_type::xfs },
                { internal::fs_magic::ext2, fs_type::ext2 },
                { internal::fs_magic::ext3, fs_type::ext3 },
                { internal::fs_magic::ext4, fs_type::ext4 },
                { internal::fs_magic::btrfs, fs_type::btrfs },
                { internal::fs_magic::hfs, fs_type::hfs },
                { internal::fs_magic::tmpfs, fs_type::tmpfs },
            };
            sr.throw_fs_exception_if_error("statfs failed", pathname);

            fs_type ret = fs_type::other;
            if (type_mapper.count(sr.extra.f_type) != 0) {
                ret = type_mapper.at(sr.extra.f_type);
            }
            return make_ready_future<fs_type>(ret);
        });
    });
}

future<struct statfs>
reactor::fstatfs(int fd) noexcept {
    return _thread_pool->submit<syscall_result_extra<struct statfs>>([fd] {
        struct statfs st;
        auto ret = ::fstatfs(fd, &st);
        return wrap_syscall(ret, st);
    }).then([] (syscall_result_extra<struct statfs> sr) {
        sr.throw_if_error();
        struct statfs st = sr.extra;
        return make_ready_future<struct statfs>(std::move(st));
    });
}

future<std::filesystem::space_info>
reactor::file_system_space(std::string_view pathname) noexcept {
    auto sr = co_await _thread_pool->submit<syscall_result_extra<std::filesystem::space_info>>([path = std::filesystem::path(pathname)] {
        std::error_code ec;
        auto si = std::filesystem::space(path, ec);
        return wrap_syscall(ec.value(), si);
    });
    sr.throw_fs_exception_if_error("std::filesystem::space failed", sstring(pathname));
    co_return sr.extra;
}

future<struct statvfs>
reactor::statvfs(std::string_view pathname) noexcept {
    // Allocating memory for a sstring can throw, hence the futurize_invoke
    return futurize_invoke([pathname, this] {
        return _thread_pool->submit<syscall_result_extra<struct statvfs>>([pathname = sstring(pathname)] {
            struct statvfs st;
            auto ret = ::statvfs(pathname.c_str(), &st);
            return wrap_syscall(ret, st);
        }).then([pathname = sstring(pathname)] (syscall_result_extra<struct statvfs> sr) {
            sr.throw_fs_exception_if_error("statvfs failed", pathname);
            struct statvfs st = sr.extra;
            return make_ready_future<struct statvfs>(std::move(st));
        });
    });
}

future<file>
reactor::open_directory(std::string_view name) noexcept {
    // Allocating memory for a sstring can throw, hence the futurize_invoke
    return futurize_invoke([name, this] {
        auto oflags = O_DIRECTORY | O_CLOEXEC | O_RDONLY;
        return _thread_pool->submit<syscall_result_extra<struct stat>>([name = sstring(name), oflags] {
            struct stat st;
            int fd = ::open(name.c_str(), oflags);
            if (fd != -1) {
                int r = ::fstat(fd, &st);
                if (r == -1) {
                    ::close(fd);
                    fd = r;
                }
            }
            return wrap_syscall(fd, st);
        }).then([name = sstring(name), oflags] (syscall_result_extra<struct stat> sr) {
            sr.throw_fs_exception_if_error("open failed", name);
            return make_file_impl(sr.result, file_open_options(), oflags, sr.extra);
        }).then([] (shared_ptr<file_impl> file_impl) {
            return make_ready_future<file>(std::move(file_impl));
        });
    });
}

future<>
reactor::make_directory(std::string_view name, file_permissions permissions) noexcept {
    // Allocating memory for a sstring can throw, hence the futurize_invoke
    return futurize_invoke([name, permissions, this] {
        return _thread_pool->submit<syscall_result<int>>([name = sstring(name), permissions] {
            auto mode = static_cast<mode_t>(permissions);
            return wrap_syscall<int>(::mkdir(name.c_str(), mode));
        }).then([name = sstring(name)] (syscall_result<int> sr) {
            sr.throw_fs_exception_if_error("mkdir failed", name);
        });
    });
}

future<>
reactor::touch_directory(std::string_view name, file_permissions permissions) noexcept {
    // Allocating memory for a sstring can throw, hence the futurize_invoke
    return futurize_invoke([this, name, permissions] {
        return _thread_pool->submit<syscall_result<int>>([name = sstring(name), permissions] {
            auto mode = static_cast<mode_t>(permissions);
            return wrap_syscall<int>(::mkdir(name.c_str(), mode));
        }).then([name = sstring(name)] (syscall_result<int> sr) {
            if (sr.result == -1 && sr.error != EEXIST) {
                sr.throw_fs_exception("mkdir failed", fs::path(name));
            }
            return make_ready_future<>();
        });
    });
}

future<>
reactor::fdatasync(int fd) noexcept {
    ++_fsyncs;
    if (_cfg.bypass_fsync) {
        return make_ready_future<>();
    }
    if (_cfg.have_aio_fsync) {
        // Does not go through the I/O queue, but has to be deleted
        struct fsync_io_desc final : public io_completion {
            promise<> _pr;
        public:
            virtual void complete(size_t res) noexcept override {
                _pr.set_value();
                delete this;
            }

            virtual void set_exception(std::exception_ptr eptr) noexcept override {
                _pr.set_exception(std::move(eptr));
                delete this;
            }

            future<> get_future() {
                return _pr.get_future();
            }
        };

        return futurize_invoke([this, fd] {
            auto desc = new fsync_io_desc;
            auto fut = desc->get_future();
            auto req = internal::io_request::make_fdatasync(fd);
            _io_sink.submit(desc, std::move(req));
            return fut;
        });
    }
    return _thread_pool->submit<syscall_result<int>>([fd] {
        return wrap_syscall<int>(::fdatasync(fd));
    }).then([] (syscall_result<int> sr) {
        sr.throw_if_error();
        return make_ready_future<>();
    });
}

// Note: terminate if arm_highres_timer throws
// `when` should always be valid
void reactor::enable_timer(steady_clock_type::time_point when) noexcept
{
    itimerspec its;
    its.it_interval = {};
    its.it_value = to_timespec(when);
    _backend->arm_highres_timer(its);
}


void reactor::add_timer(timer<steady_clock_type>* tmr) noexcept {
    if (queue_timer(tmr)) {
        enable_timer(_timers.get_next_timeout());
    }
}

bool reactor::queue_timer(timer<steady_clock_type>* tmr) noexcept {
    return _timers.insert(*tmr);
}

void reactor::del_timer(timer<steady_clock_type>* tmr) noexcept {
    _timers.remove(*tmr, _expired_timers);
}

void reactor::add_timer(timer<lowres_clock>* tmr) noexcept {
    if (queue_timer(tmr)) {
        _lowres_next_timeout = _lowres_timers.get_next_timeout();
    }
}

bool reactor::queue_timer(timer<lowres_clock>* tmr) noexcept {
    return _lowres_timers.insert(*tmr);
}

void reactor::del_timer(timer<lowres_clock>* tmr) noexcept {
    _lowres_timers.remove(*tmr, _expired_lowres_timers);
}

void reactor::add_timer(timer<manual_clock>* tmr) noexcept {
    queue_timer(tmr);
}

bool reactor::queue_timer(timer<manual_clock>* tmr) noexcept {
    return _manual_timers.insert(*tmr);
}

void reactor::del_timer(timer<manual_clock>* tmr) noexcept {
    _manual_timers.remove(*tmr, _expired_manual_timers);
}

void reactor::at_exit(noncopyable_function<future<> ()> func) {
    SEASTAR_ASSERT(!_stopping);
    _exit_funcs.push_back(std::move(func));
}

future<> reactor::run_exit_tasks() {
    _stop_requested.broadcast();
    stop_aio_eventfd_loop();
    return do_for_each(_exit_funcs.rbegin(), _exit_funcs.rend(), [] (auto& func) {
        return func();
    });
}

void reactor::stop() {
    SEASTAR_ASSERT(_id == 0);
    _smp->cleanup_cpu();
    if (!std::exchange(_stopping, true)) {
        // Run exit tasks locally and then stop all other engines
        // in the background and wait on semaphore for all to complete.
        // Finally, set _stopped on cpu 0.
        (void)drain().then([this] {
          return run_exit_tasks().then([this] {
            return do_with(semaphore(0), [this] (semaphore& sem) {
                // Stop other cpus asynchronously, signal when done.
                (void)smp::invoke_on_others(0, [] {
                    engine()._smp->cleanup_cpu();
                    engine()._stopping = true;
                    return engine().run_exit_tasks().then([] {
                        engine()._stopped = true;
                    });
                }).then([&sem]() {
                    sem.signal();
                });
                return sem.wait().then([this] {
                    _stopped = true;
                });
            });
          });
        });
    }
}

void reactor::exit(int ret) {
    // Run stop() asynchronously on cpu 0.
    (void)smp::submit_to(0, [this, ret] { _return = ret; stop(); });
}

uint64_t
reactor::pending_task_count() const {
    uint64_t ret = 0;
    for (auto&& tq : _task_queues) {
        if (tq) {
            ret += tq->_q.size();
        }
    }
    return ret;
}

uint64_t
reactor::tasks_processed() const {
    return _global_tasks_processed;
}

void reactor::register_metrics() {

    namespace sm = seastar::metrics;

    _metric_groups.add_group("reactor", {
            sm::make_gauge("tasks_pending", std::bind(&reactor::pending_task_count, this), sm::description("Number of pending tasks in the queue")),
            // total_operations value:DERIVE:0:U
            sm::make_counter("tasks_processed", std::bind(&reactor::tasks_processed, this), sm::description("Total tasks processed")),
            sm::make_counter("polls", _polls, sm::description("Number of times pollers were executed")),
            sm::make_gauge("timers_pending", std::bind(&decltype(_timers)::size, &_timers), sm::description("Number of tasks in the timer-pending queue")),
            sm::make_gauge("utilization", [this] { return (1-_load)  * 100; }, sm::description("CPU utilization")),
            sm::make_counter("cpu_busy_ms", [this] () -> int64_t { return total_busy_time() / 1ms; },
                    sm::description("Total cpu busy time in milliseconds")),
            sm::make_counter("sleep_time_ms_total", [this] () -> int64_t { return _total_sleep / 1ms; },
                    sm::description("Total reactor sleep time (wall clock)")),
            sm::make_counter("awake_time_ms_total", [this] () -> int64_t { return total_awake_time() / 1ms; },
                    sm::description("Total reactor awake time (wall_clock)")),
            sm::make_counter("cpu_used_time_ms", [this] () -> int64_t { return total_cpu_time() / 1ms; },
                    sm::description("Total reactor thread CPU time (from CLOCK_THREAD_CPUTIME)")),
            sm::make_counter("cpu_steal_time_ms", [this] () -> int64_t { return total_steal_time() / 1ms; },
                    sm::description("Total steal time, the time in which something else was running while the reactor was runnable (not sleeping)."
                                     "Because this is in userspace, some time that could be legitimally thought as steal time is not accounted as such. For example, if we are sleeping and can wake up but the kernel hasn't woken us up yet.")),
            // total_operations value:DERIVE:0:U
            sm::make_counter("aio_reads", _io_stats.aio_reads, sm::description("Total aio-reads operations")),

            sm::make_total_bytes("aio_bytes_read", _io_stats.aio_read_bytes, sm::description("Total aio-reads bytes")),
            // total_operations value:DERIVE:0:U
            sm::make_counter("aio_writes", _io_stats.aio_writes, sm::description("Total aio-writes operations")),
            sm::make_total_bytes("aio_bytes_write", _io_stats.aio_write_bytes, sm::description("Total aio-writes bytes")),
            sm::make_counter("aio_outsizes", _io_stats.aio_outsizes, sm::description("Total number of aio operations that exceed IO limit")),
            sm::make_counter("aio_errors", _io_stats.aio_errors, sm::description("Total aio errors")),
            sm::make_histogram("stalls", sm::description("A histogram of reactor stall durations"), [this] {return _stalls_histogram.to_metrics_histogram();}).aggregate({seastar::metrics::shard_label}).set_skip_when_empty(),
            // total_operations value:DERIVE:0:U
            sm::make_counter("fsyncs", _fsyncs, sm::description("Total number of fsync operations")),
            // total_operations value:DERIVE:0:U
            sm::make_counter("io_threaded_fallbacks", std::bind(&thread_pool::operation_count, _thread_pool.get()),
                    sm::description("Total number of io-threaded-fallbacks operations")),

    });

    _metric_groups.add_group("memory", {
            sm::make_counter("malloc_operations", [] { return memory::stats().mallocs(); },
                    sm::description("Total number of malloc operations")),
            sm::make_counter("free_operations", [] { return memory::stats().frees(); }, sm::description("Total number of free operations")),
            sm::make_counter("cross_cpu_free_operations", [] { return memory::stats().cross_cpu_frees(); }, sm::description("Total number of cross cpu free")),
            sm::make_gauge("malloc_live_objects", [] { return memory::stats().live_objects(); }, sm::description("Number of live objects")),
            sm::make_current_bytes("free_memory", [] { return memory::stats().free_memory(); }, sm::description("Free memory size in bytes")),
            sm::make_current_bytes("total_memory", [] { return memory::stats().total_memory(); }, sm::description("Total memory size in bytes")),
            sm::make_current_bytes("allocated_memory", [] { return memory::stats().allocated_memory(); }, sm::description("Allocated memory size in bytes")),
            sm::make_counter("reclaims_operations", [] { return memory::stats().reclaims(); }, sm::description("Total reclaims operations")),
            sm::make_counter("malloc_failed", [] { return memory::stats().failed_allocations(); }, sm::description("Total count of failed memory allocations"))
    });

    _metric_groups.add_group("reactor", {
            sm::make_counter("logging_failures", [] { return logging_failures; }, sm::description("Total number of logging failures")),
            // total_operations value:DERIVE:0:U
            sm::make_counter("cpp_exceptions", _cxx_exceptions, sm::description("Total number of C++ exceptions")),
            sm::make_counter("abandoned_failed_futures", _abandoned_failed_futures, sm::description("Total number of abandoned failed futures, futures destroyed while still containing an exception")),
    });

    _metric_groups.add_group("reactor", {
        sm::make_counter("fstream_reads", _io_stats.fstream_reads,
                sm::description(
                        "Counts reads from disk file streams.  A high rate indicates high disk activity."
                        " Contrast with other fstream_read* counters to locate bottlenecks.")),
        sm::make_counter("fstream_read_bytes", _io_stats.fstream_read_bytes,
                sm::description(
                        "Counts bytes read from disk file streams.  A high rate indicates high disk activity."
                        " Divide by fstream_reads to determine average read size.")),
        sm::make_counter("fstream_reads_blocked", _io_stats.fstream_reads_blocked,
                sm::description(
                        "Counts the number of times a disk read could not be satisfied from read-ahead buffers, and had to block."
                        " Indicates short streams, or incorrect read ahead configuration.")),
        sm::make_counter("fstream_read_bytes_blocked", _io_stats.fstream_read_bytes_blocked,
                sm::description(
                        "Counts the number of bytes read from disk that could not be satisfied from read-ahead buffers, and had to block."
                        " Indicates short streams, or incorrect read ahead configuration.")),
        sm::make_counter("fstream_reads_aheads_discarded", _io_stats.fstream_read_aheads_discarded,
                sm::description(
                        "Counts the number of times a buffer that was read ahead of time and was discarded because it was not needed, wasting disk bandwidth."
                        " Indicates over-eager read ahead configuration.")),
        sm::make_counter("fstream_reads_ahead_bytes_discarded", _io_stats.fstream_read_ahead_discarded_bytes,
                sm::description(
                        "Counts the number of buffered bytes that were read ahead of time and were discarded because they were not needed, wasting disk bandwidth."
                        " Indicates over-eager read ahead configuration.")),
    });
}

seastar::internal::log_buf::inserter_iterator do_dump_task_queue(seastar::internal::log_buf::inserter_iterator it, const reactor::task_queue& tq) {
    memory::scoped_critical_alloc_section _;
    std::unordered_map<const char*, unsigned> infos;
    for (const auto& tp : tq._q) {
        const std::type_info& ti = typeid(*tp);
        auto [ it, ins ] = infos.emplace(std::make_pair(ti.name(), 0u));
        it->second++;
    }
    it = fmt::format_to(it, "Too long queue accumulated for {} ({} tasks)\n", tq._name, tq._q.size());
    for (auto& ti : infos) {
        it = fmt::format_to(it, " {}: {}\n", ti.second, ti.first);
    }
    return it;
}

void reactor::run_tasks(task_queue& tq) {
    // Make sure new tasks will inherit our scheduling group
    *internal::current_scheduling_group_ptr() = scheduling_group(tq._id);
    auto& tasks = tq._q;
    while (!tasks.empty()) {
        auto tsk = tasks.front();
        tasks.pop_front();
        STAP_PROBE(seastar, reactor_run_tasks_single_start);
        internal::task_histogram_add_task(*tsk);
        _current_task = tsk;
        tsk->run_and_dispose();
        _current_task = nullptr;
        STAP_PROBE(seastar, reactor_run_tasks_single_end);
        ++tq._tasks_processed;
        ++_global_tasks_processed;
        // check at end of loop, to allow at least one task to run
        if (internal::scheduler_need_preempt()) {
            if (tasks.size() <= _cfg.max_task_backlog) {
                break;
            } else {
                // While need_preempt() is set, task execution is inefficient due to
                // need_preempt() checks breaking out of loops and .then() calls. See
                // #302.
                reset_preemption_monitor();
                lowres_clock::update();

                static thread_local logger::rate_limit rate_limit(std::chrono::seconds(10));
                logger::lambda_log_writer writer([&tq] (auto it) { return do_dump_task_queue(it, tq); });
                seastar_logger.log(log_level::warn, rate_limit, writer);
            }
        }
    }
}

namespace {

#ifdef SEASTAR_SHUFFLE_TASK_QUEUE
void shuffle(task*& t, circular_buffer<task*>& q) {
    static thread_local std::mt19937 gen = std::mt19937(std::default_random_engine()());
    std::uniform_int_distribution<size_t> tasks_dist{0, q.size() - 1};
    auto& to_swap = q[tasks_dist(gen)];
    std::swap(to_swap, t);
}
#else
void shuffle(task*&, circular_buffer<task*>&) {
}
#endif

}

void reactor::force_poll() {
    request_preemption();
}

bool
reactor::flush_tcp_batches() {
    bool work = !_flush_batching.empty();
    while (!_flush_batching.empty()) {
        auto& os = _flush_batching.front();
        _flush_batching.pop_front();
        os.poll_flush();
    }
    return work;
}

bool
reactor::do_expire_lowres_timers() noexcept {
    auto now = lowres_clock::now();
    if (now >= _lowres_next_timeout) {
        _lowres_timers.complete(_expired_lowres_timers, [this] () noexcept {
            if (!_lowres_timers.empty()) {
                _lowres_next_timeout = _lowres_timers.get_next_timeout();
            } else {
                _lowres_next_timeout = lowres_clock::time_point::max();
            }
        });
        return true;
    }
    return false;
}

void
reactor::expire_manual_timers() noexcept {
    _manual_timers.complete(_expired_manual_timers, [] () noexcept {});
}

void
manual_clock::expire_timers() noexcept {
    local_engine->expire_manual_timers();
}

void
manual_clock::advance(manual_clock::duration d) noexcept {
    _now.fetch_add(d.count());
    if (local_engine) {
        // Expire timers on all cores in the background.
        local_engine->run_in_background([] {
            schedule_urgent(make_task(default_scheduling_group(), &manual_clock::expire_timers));
            return smp::invoke_on_all(&manual_clock::expire_timers);
        });
    }
}

bool
reactor::do_check_lowres_timers() const noexcept {
    return lowres_clock::now() > _lowres_next_timeout;
}

class reactor::kernel_submit_work_pollfn final : public simple_pollfn<true> {
    reactor& _r;
public:
    kernel_submit_work_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() override final {
        return _r._backend->kernel_submit_work();
    }
};

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

class reactor::batch_flush_pollfn final : public simple_pollfn<true> {
    reactor& _r;
public:
    batch_flush_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() final override {
        return _r.flush_tcp_batches();
    }
};

class reactor::reap_kernel_completions_pollfn final : public reactor::pollfn {
    reactor& _r;
public:
    reap_kernel_completions_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() final override {
        return _r.reap_kernel_completions();
    }
    virtual bool pure_poll() override final {
        return poll(); // actually performs work, but triggers no user continuations, so okay
    }
    virtual bool try_enter_interrupt_mode() override {
        return _r._backend->kernel_events_can_sleep();
    }
    virtual void exit_interrupt_mode() override final {
    }
};

class reactor::io_queue_submission_pollfn final : public reactor::pollfn {
    reactor& _r;
    // Wake-up the reactor with highres timer when the io-queue
    // decides to delay dispatching until some time point in
    // the future
    timer<> _nearest_wakeup { [this] { _armed = false; } };
    bool _armed = false;
public:
    io_queue_submission_pollfn(reactor& r) : _r(r) {}
    virtual bool poll() final override {
        return _r.flush_pending_aio();
    }
    virtual bool pure_poll() override final {
        return poll();
    }
    virtual bool try_enter_interrupt_mode() override {
        auto next = _r.next_pending_aio();
        auto now = steady_clock_type::now();
        if (next <= now) {
            return false;
        }
        _nearest_wakeup.arm(next);
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

// Other cpus can queue items for us to free; and they won't notify
// us about them.  But it's okay to ignore those items, freeing them
// doesn't have any side effects.
//
// We'll take care of those items when we wake up for another reason.
class reactor::drain_cross_cpu_freelist_pollfn final : public simple_pollfn<true> {
public:
    virtual bool poll() final override {
        return memory::drain_cross_cpu_freelist();
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
        if (next == lowres_clock::time_point::max()) {
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
        // Avoid short-circuiting with `||` since there are side effects
        // we want to take place (instantiating tasks from the alien queue).
        // Cast to int to silence gcc -Wbitwise-instead-of-logical.
        return (int(smp::poll_queues()) |
                int(_r._alien.poll_queues()));
    }
    virtual bool pure_poll() final override {
        return (smp::pure_poll_queues() ||
                _r._alien.pure_poll_queues());
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
        return _r._thread_pool->complete();
    }
    virtual bool pure_poll() override final {
        return poll(); // actually performs work, but triggers no user continuations, so okay
    }
    virtual bool try_enter_interrupt_mode() override {
        _r._thread_pool->enter_interrupt_mode();
        if (poll()) {
            // raced
            _r._thread_pool->exit_interrupt_mode();
            return false;
        }
        return true;
    }
    virtual void exit_interrupt_mode() override final {
        _r._thread_pool->exit_interrupt_mode();
    }
};

void
reactor::wakeup() {
    if (!_sleeping.load(std::memory_order_relaxed)) {
        return;
    }

    // We are free to clear it, because we're sending a signal now
    _sleeping.store(false, std::memory_order_relaxed);

    uint64_t one = 1;
    auto res = ::write(_notify_eventfd.get(), &one, sizeof(one));
    SEASTAR_ASSERT(res == sizeof(one) && "write(2) failed on _reactor._notify_eventfd");
}

void reactor::start_aio_eventfd_loop() {
    if (!_aio_eventfd) {
        return;
    }
    future<> loop_done = repeat([this] {
        return _aio_eventfd->readable().then([this] {
            char garbage[8];
            std::ignore = ::read(_aio_eventfd->get_fd(), garbage, 8); // totally uninteresting
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
    auto res = ::write(_aio_eventfd->get_fd(), &one, 8);
    SEASTAR_ASSERT(res == 8 && "write(2) failed on _reactor._aio_eventfd");
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

reactor::task_queue* reactor::pop_active_task_queue(sched_clock::time_point now) {
    task_queue* tq = _active_task_queues.front();
    _active_task_queues.pop_front();
    tq->_starvetime += now - tq->_ts;
    return tq;
}

void
reactor::insert_activating_task_queues() {
    // Quadratic, but since we expect the common cases in insert_active_task_queue() to dominate, faster
    for (auto&& tq : _activating_task_queues) {
        insert_active_task_queue(tq);
    }
    _activating_task_queues.clear();
}

void reactor::add_task(task* t) noexcept {
    auto sg = t->group();
    auto* q = _task_queues[sg._id].get();
    bool was_empty = q->_q.empty();
    q->_q.push_back(std::move(t));
    shuffle(q->_q.back(), q->_q);
    if (was_empty) {
        activate(*q);
    }
}

void reactor::add_urgent_task(task* t) noexcept {
    memory::scoped_critical_alloc_section _;
    auto sg = t->group();
    auto* q = _task_queues[sg._id].get();
    bool was_empty = q->_q.empty();
    q->_q.push_front(std::move(t));
    shuffle(q->_q.front(), q->_q);
    if (was_empty) {
        activate(*q);
    }
}

void reactor::run_in_background(future<> f) {
    try {
        // _backgroud_gate closed in reactor::close()
        (void)with_gate(_background_gate, [f = std::move(f)] () mutable {
            return f.handle_exception([] (std::exception_ptr ex) {
                seastar_logger.warn("Ignored background task failure: {}", std::move(ex));
            });
        });
    } catch (...) {
        // Swallow gate_closed_exception in particular
        seastar_logger.error("run_in_background: {}", std::current_exception());
    }
}

future<> reactor::drain() {
    seastar_logger.debug("reactor::drain");
    return smp::invoke_on_all([] {
        if (engine()._background_gate.is_closed()) {
            return make_ready_future<>();
        }
        return engine()._background_gate.close();
    });
}

void
reactor::run_some_tasks() {
    if (!have_more_tasks()) {
        return;
    }
    sched_print("run_some_tasks: start");
    reset_preemption_monitor();
    lowres_clock::update();

    sched_clock::time_point t_run_completed = now();
    STAP_PROBE(seastar, reactor_run_tasks_start);
    _cpu_stall_detector->start_task_run(t_run_completed);
    do {
        auto t_run_started = t_run_completed;
        insert_activating_task_queues();
        task_queue* tq = pop_active_task_queue(t_run_started);
        sched_print("running tq {} {}", (void*)tq, tq->_name);
        _last_vruntime = std::max(tq->_vruntime, _last_vruntime);
        run_tasks(*tq);
        t_run_completed = now();
        auto delta = t_run_completed - t_run_started;
        account_runtime(*tq, delta);
        sched_print("run complete ({} {}); time consumed {} usec; final vruntime {} empty {}",
                (void*)tq, tq->_name, delta / 1us, tq->_vruntime, tq->_q.empty());
        tq->_ts = t_run_completed;
        if (!tq->_q.empty()) {
            insert_active_task_queue(tq);
        } else {
            tq->_active = false;
        }
        // We must not use internal::scheduler_need_preempt() below,
        // since in debug mode we'll never have two successive calls
        // to internal::scheduler_need_preempt() both return true, so
        // an infinite loop with yield() will never terminate.
        //
        // Settle on a regular need_preempt(), which will return true in
        // debug mode.
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
    auto now = reactor::now();
    tq._waittime += now - tq._ts;
    tq._ts = now;
    _activating_task_queues.push_back(&tq);
}

void reactor::service_highres_timer() noexcept {
    _timers.complete(_expired_timers, [this] () noexcept {
        if (!_timers.empty()) {
            enable_timer(_timers.get_next_timeout());
        }
    });
}

int reactor::run() noexcept {
    try {
        return do_run();
    } catch (const std::exception& e) {
        seastar_logger.error("{}", e.what());
        print_with_backtrace("exception running reactor main loop");
        _exit(1);
    }
}

int reactor::do_run() {
    auto signal_stack = install_signal_handler_stack();

    register_metrics();

    // The order in which we execute the pollers is very important for performance.
    //
    // This is because events that are generated in one poller may feed work into others. If
    // they were reversed, we'd only be able to do that work in the next task quota.
    //
    // One example is the relationship between the smp poller and the I/O submission poller:
    // If the smp poller runs first, requests from remote I/O queues can be dispatched right away
    //
    // We will run the pollers in the following order:
    //
    // 1. SMP: any remote event arrives before anything else
    // 2. reap kernel events completion: storage related completions may free up space in the I/O
    //                                   queue.
    // 4. I/O queue: must be after reap, to free up events. If new slots are freed may submit I/O
    // 5. kernel submission: for I/O, will submit what was generated from last step.
    // 6. reap kernel events completion: some of the submissions from last step may return immediately.
    //                                   For example if we are dealing with poll() on a fd that has events.
    poller smp_poller(std::make_unique<smp_pollfn>(*this));

    poller reap_kernel_completions_poller(std::make_unique<reap_kernel_completions_pollfn>(*this));
    poller io_queue_submission_poller(std::make_unique<io_queue_submission_pollfn>(*this));
    poller kernel_submit_work_poller(std::make_unique<kernel_submit_work_pollfn>(*this));
    poller final_real_kernel_completions_poller(std::make_unique<reap_kernel_completions_pollfn>(*this));

    poller batch_flush_poller(std::make_unique<batch_flush_pollfn>(*this));
    poller execution_stage_poller(std::make_unique<execution_stage_pollfn>());

    start_aio_eventfd_loop();

    if (_id == 0 && _cfg.auto_handle_sigint_sigterm) {
       if (_cfg.handle_sigint) {
          _signals.handle_signal_once(SIGINT, [this] { stop(); });
       }
       _signals.handle_signal_once(SIGTERM, [this] { stop(); });
    }

    // Start initialization in the background.
    // Communicate when done using _start_promise.
    (void)_cpu_started.wait(smp::count).then([this] {
        (void)_network_stack->initialize().then([this] {
            _start_promise.set_value();
        });
    });
    // Wait for network stack in the background and then signal all cpus.
    (void)_network_stack_ready->then([this] (std::unique_ptr<network_stack> stack) {
        _network_stack = std::move(stack);
        return smp::invoke_on_all([] {
            engine()._cpu_started.signal();
        });
    });

    poller syscall_poller(std::make_unique<syscall_pollfn>(*this));

    poller drain_cross_cpu_freelist(std::make_unique<drain_cross_cpu_freelist_pollfn>());

    poller expire_lowres_timers(std::make_unique<lowres_timer_pollfn>(*this));
    poller sig_poller(std::make_unique<signal_pollfn>(*this));

    using namespace std::chrono_literals;
    timer<lowres_clock> load_timer;
    auto last_idle = _total_idle;
    auto idle_start = now(), idle_end = idle_start;
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

    itimerspec its = seastar::posix::to_relative_itimerspec(_cfg.task_quota, _cfg.task_quota);
    _task_quota_timer.timerfd_settime(0, its);
    auto& task_quote_itimerspec = its;

    struct sigaction sa_block_notifier = {};
    sa_block_notifier.sa_handler = &reactor::block_notifier;
    sa_block_notifier.sa_flags = SA_RESTART;
    auto r = sigaction(internal::cpu_stall_detector::signal_number(), &sa_block_notifier, nullptr);
    SEASTAR_ASSERT(r == 0);

    bool idle = false;

    std::function<bool()> check_for_work = [this] () {
        return poll_once() || have_more_tasks();
    };
    std::function<bool()> pure_check_for_work = [this] () {
        return pure_poll_once() || have_more_tasks();
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
            _finished_running_tasks = true;
            _smp->arrive_at_event_loop_end();
            if (_id == 0) {
                _smp->join_all();
            }
            break;
        }

        _polls++;

        lowres_clock::update(); // Don't delay expiring lowres timers
        if (check_for_work()) {
            if (idle) {
                _total_idle += idle_end - idle_start;
                idle_start = idle_end;
                idle = false;
            }
        } else {
            idle_end = now();
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
                if (idle_end - idle_start > _cfg.max_poll_time) {
                    // Turn off the task quota timer to avoid spurious wakeups
                    struct itimerspec zero_itimerspec = {};
                    _task_quota_timer.timerfd_settime(0, zero_itimerspec);
                    _cpu_stall_detector->start_sleep();
                    try_sleep();
                    _cpu_stall_detector->end_sleep();
                    // We may have slept for a while, so freshen idle_end
                    idle_end = now();
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
    _io_queues.clear();
    return _return;
}


void
reactor::try_sleep() {
    for (auto i = _pollers.begin(); i != _pollers.end(); ++i) {
        auto ok = (*i)->try_enter_interrupt_mode();
        if (!ok) {
            while (i != _pollers.begin()) {
                (*--i)->exit_interrupt_mode();
            }
            return;
        }
    }

    _backend->wait_and_process_events(&_active_sigmask);

    for (auto i = _pollers.rbegin(); i != _pollers.rend(); ++i) {
        (*i)->exit_interrupt_mode();
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

namespace internal {

class poller::registration_task final : public task {
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
    task* waiting_task() noexcept override { return nullptr; }
    void cancel() {
        _p = nullptr;
    }
    void moved(poller* p) {
        _p = p;
    }
};

class poller::deregistration_task final : public task {
private:
    std::unique_ptr<pollfn> _p;
public:
    explicit deregistration_task(std::unique_ptr<pollfn>&& p) : _p(std::move(p)) {}
    virtual void run_and_dispose() noexcept override {
        engine().unregister_poller(_p.get());
        delete this;
    }
    task* waiting_task() noexcept override { return nullptr; }
};

}

void reactor::register_poller(pollfn* p) {
    _pollers.push_back(p);
}

void reactor::unregister_poller(pollfn* p) {
    _pollers.erase(std::find(_pollers.begin(), _pollers.end(), p));
}

void reactor::replace_poller(pollfn* old, pollfn* neww) {
    std::replace(_pollers.begin(), _pollers.end(), old, neww);
}

namespace internal {

poller::poller(poller&& x) noexcept
        : _pollfn(std::move(x._pollfn)), _registration_task(std::exchange(x._registration_task, nullptr)) {
    if (_pollfn && _registration_task) {
        _registration_task->moved(this);
    }
}

poller&
poller::operator=(poller&& x) noexcept {
    if (this != &x) {
        this->~poller();
        new (this) poller(std::move(x));
    }
    return *this;
}

void
poller::do_register() noexcept {
    // We can't just insert a poller into reactor::_pollers, because we
    // may be running inside a poller ourselves, and so in the middle of
    // iterating reactor::_pollers itself.  So we schedule a task to add
    // the poller instead.
    auto task = new registration_task(this);
    engine().add_task(task);
    _registration_task = task;
}

poller::~poller() {
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
        } else if (!engine()._finished_running_tasks) {
            // If _finished_running_tasks, the call to add_task() below will just
            // leak it, since no one will call task::run_and_dispose(). Just leave
            // the poller there, the reactor will never use it.
            auto dummy = make_pollfn([] { return false; });
            auto dummy_p = dummy.get();
            auto task = new deregistration_task(std::move(dummy));
            engine().add_task(task);
            engine().replace_poller(_pollfn.get(), dummy_p);
        }
    }
}

}

syscall_work_queue::syscall_work_queue()
    : _pending()
    , _completed()
    , _start_eventfd(0) {
}

void syscall_work_queue::submit_item(std::unique_ptr<syscall_work_queue::work_item> item) {
    (void)_queue_has_room.wait().then_wrapped([this, item = std::move(item)] (future<> f) mutable {
        // propagate wait failure via work_item
        if (f.failed()) {
            item->set_exception(f.get_exception());
            return;
        }
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

void smp_message_queue::submit_item(shard_id t, smp_timeout_clock::time_point timeout, std::unique_ptr<smp_message_queue::work_item> item) {
  // matching signal() in process_completions()
  auto ssg_id = internal::smp_service_group_id(item->ssg);
  auto& sem = get_smp_service_groups_semaphore(ssg_id, t);
  // Future indirectly forwarded to `item`.
  (void)get_units(sem, 1, timeout).then_wrapped([this, item = std::move(item)] (future<smp_service_group_semaphore_units> units_fut) mutable {
    if (units_fut.failed()) {
        item->fail_with(units_fut.get_exception());
        ++_compl;
        ++_last_cmpl_batch;
        return;
    }
    _tx.a.pending_fifo.push_back(item.get());
    // no exceptions from this point
    item.release();
    units_fut.get().release();
    if (_tx.a.pending_fifo.size() >= batch_size) {
        move_pending();
    }
  });
}

void smp_message_queue::respond(work_item* item) {
    _completed_fifo.push_back(item);
    if (_completed_fifo.size() >= batch_size || engine().stopped()) {
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
    remote->wakeup();
}

smp_message_queue::lf_queue::~lf_queue() {
    consume_all([] (work_item* ptr) {
        delete ptr;
    });
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

size_t smp_message_queue::process_completions(shard_id t) {
    auto nr = process_queue<prefetch_cnt*2>(_completed, [t] (work_item* wi) {
        wi->complete();
        auto ssg_id = internal::smp_service_group_id(wi->ssg);
        get_smp_service_groups_semaphore(ssg_id, t).signal();
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
    std::snprintf(instance, sizeof(instance), "%u-%u", this_shard_id(), cpuid);
    _metrics.add_group("smp", {
            // queue_length     value:GAUGE:0:U
            // Absolute value of num packets in last tx batch.
            sm::make_queue_length("send_batch_queue_length", _last_snt_batch, sm::description("Current send batch queue length"), {sm::shard_label(instance)})(sm::metric_disabled),
            sm::make_queue_length("receive_batch_queue_length", _last_rcv_batch, sm::description("Current receive batch queue length"), {sm::shard_label(instance)})(sm::metric_disabled),
            sm::make_queue_length("complete_batch_queue_length", _last_cmpl_batch, sm::description("Current complete batch queue length"), {sm::shard_label(instance)})(sm::metric_disabled),
            sm::make_queue_length("send_queue_length", _current_queue_length, sm::description("Current send queue length"), {sm::shard_label(instance)})(sm::metric_disabled),
            // total_operations value:DERIVE:0:U
            sm::make_counter("total_received_messages", _received, sm::description("Total number of received messages"), {sm::shard_label(instance)})(sm::metric_disabled),
            // total_operations value:DERIVE:0:U
            sm::make_counter("total_sent_messages", _sent, sm::description("Total number of sent messages"), {sm::shard_label(instance)})(sm::metric_disabled),
            // total_operations value:DERIVE:0:U
            sm::make_counter("total_completed_messages", _compl, sm::description("Total number of messages completed"), {sm::shard_label(instance)})(sm::metric_disabled)
    });
}

readable_eventfd writeable_eventfd::read_side() {
    return readable_eventfd(_fd.dup());
}

file_desc writeable_eventfd::try_create_eventfd(size_t initial) {
    SEASTAR_ASSERT(size_t(int(initial)) == initial);
    return file_desc::eventfd(initial, EFD_CLOEXEC);
}

void writeable_eventfd::signal(size_t count) {
    uint64_t c = count;
    auto r = _fd.write(&c, sizeof(c));
    SEASTAR_ASSERT(r == sizeof(c));
}

writeable_eventfd readable_eventfd::write_side() {
    return writeable_eventfd(_fd.get_file_desc().dup());
}

file_desc readable_eventfd::try_create_eventfd(size_t initial) {
    SEASTAR_ASSERT(size_t(int(initial)) == initial);
    return file_desc::eventfd(initial, EFD_CLOEXEC | EFD_NONBLOCK);
}

future<size_t> readable_eventfd::wait() {
    return engine().readable(*_fd._s).then([this] {
        uint64_t count;
        int r = ::read(_fd.get_fd(), &count, sizeof(count));
        SEASTAR_ASSERT(r == sizeof(count));
        return make_ready_future<size_t>(count);
    });
}

void schedule(task* t) noexcept {
    engine().add_task(t);
}

void schedule_checked(task* t) noexcept {
    if (t->group().is_at_exit()) {
        // trying to schedule a task in at_destroy. Not allowed
        on_internal_error(seastar_logger, "Cannot schedule tasks in at_destroy queue. Use reactor::at_destroy.");
    }
    engine().add_task(t);
}

void schedule_urgent(task* t) noexcept {
    engine().add_urgent_task(t);
}

}

bool operator==(const ::sockaddr_in a, const ::sockaddr_in b) {
    return (a.sin_addr.s_addr == b.sin_addr.s_addr) && (a.sin_port == b.sin_port);
}

namespace seastar {

static bool kernel_supports_aio_fsync() {
    return internal::kernel_uname().whitelisted({"4.18"});
}

static program_options::selection_value<network_stack_factory> create_network_stacks_option(reactor_options& zis) {
    using value_type = program_options::selection_value<network_stack_factory>;
    value_type::candidates candidates;
    std::vector<std::string> net_stack_names;

    auto deleter = [] (network_stack_factory* p) { delete p; };

    std::string default_stack;
    for (auto reg_func : {register_native_stack, register_posix_stack}) {
        auto s = reg_func();
        if (s.is_default) {
            default_stack = s.name;
        }
        candidates.push_back({s.name, {new network_stack_factory(std::move(s.factory)), deleter}, std::move(s.opts)});
        net_stack_names.emplace_back(s.name);
    }

    return program_options::selection_value<network_stack_factory>(zis, "network-stack", std::move(candidates), default_stack,
            fmt::format("select network stack (valid values: {})", fmt::join(net_stack_names, ", ")));
}

static program_options::selection_value<reactor_backend_selector>::candidates backend_selector_candidates() {
    using value_type = program_options::selection_value<reactor_backend_selector>;
    value_type::candidates candidates;

    auto deleter = [] (reactor_backend_selector* p) { delete p; };

    for (auto&& be : reactor_backend_selector::available()) {
        auto name = be.name();
        candidates.push_back({std::move(name), {new reactor_backend_selector(std::move(be)), deleter}, {}});
    }
    return candidates;
}

reactor_options::reactor_options(program_options::option_group* parent_group)
    : program_options::option_group(parent_group, "Core options")
    , network_stack(create_network_stacks_option(*this))
    , poll_mode(*this, "poll-mode", "poll continuously (100% cpu use)")
    , idle_poll_time_us(*this, "idle-poll-time-us", reactor::calculate_poll_time() / 1us,
                "idle polling time in microseconds (reduce for overprovisioned environments or laptops)")
    , poll_aio(*this, "poll-aio", true,
                "busy-poll for disk I/O (reduces latency and increases throughput)")
    , task_quota_ms(*this, "task-quota-ms", 0.5, "Max time (ms) between polls")
    , io_latency_goal_ms(*this, "io-latency-goal-ms", {}, "Max time (ms) io operations must take (1.5 * task-quota-ms if not set)")
    , io_flow_ratio_threshold(*this, "io-flow-rate-threshold", 1.1, "Dispatch rate to completion rate threshold")
    , io_completion_notify_ms(*this, "io-completion-notify-ms", {}, "Threshold in milliseconds over which IO request completion is reported to logs")
    , max_task_backlog(*this, "max-task-backlog", 1000, "Maximum number of task backlog to allow; above this we ignore I/O")
    , blocked_reactor_notify_ms(*this, "blocked-reactor-notify-ms", 25, "threshold in miliseconds over which the reactor is considered blocked if no progress is made")
    , blocked_reactor_reports_per_minute(*this, "blocked-reactor-reports-per-minute", 5, "Maximum number of backtraces reported by stall detector per minute")
    , blocked_reactor_report_format_oneline(*this, "blocked-reactor-report-format-oneline", true, "Print a simplified backtrace on a single line")
    , relaxed_dma(*this, "relaxed-dma", "allow using buffered I/O if DMA is not available (reduces performance)")
    , linux_aio_nowait(*this, "linux-aio-nowait", aio_nowait_supported,
                "use the Linux NOWAIT AIO feature, which reduces reactor stalls due to aio (autodetected)")
    , unsafe_bypass_fsync(*this, "unsafe-bypass-fsync", false, "Bypass fsync(), may result in data loss. Use for testing on consumer drives")
    , kernel_page_cache(*this, "kernel-page-cache", false,
                "Use the kernel page cache. This disables DMA (O_DIRECT)."
                " Useful for short-lived functional tests with a small data set.")
    , overprovisioned(*this, "overprovisioned", "run in an overprovisioned environment (such as docker or a laptop); equivalent to --idle-poll-time-us 0 --thread-affinity 0 --poll-aio 0")
    , abort_on_seastar_bad_alloc(*this, "abort-on-seastar-bad-alloc", "abort when seastar allocator cannot allocate memory")
    , force_aio_syscalls(*this, "force-aio-syscalls", false,
                "Force io_getevents(2) to issue a system call, instead of bypassing the kernel when possible."
                " This makes strace output more useful, but slows down the application")
    , dump_memory_diagnostics_on_alloc_failure_kind(*this, "dump-memory-diagnostics-on-alloc-failure-kind", memory::alloc_failure_kind::critical,
                "Dump diagnostics of the seastar allocator state on allocation failure."
                 " Accepted values: none, critical (default), all. When set to critical, only allocations marked as critical will trigger diagnostics dump."
                 " The diagnostics will be written to the seastar_memory logger, with error level."
                 " Note that if the seastar_memory logger is set to debug or trace level, the diagnostics will be logged irrespective of this setting.")
    , reactor_backend(*this, "reactor-backend", backend_selector_candidates(), reactor_backend_selector::default_backend().name(),
                fmt::format("Internal reactor implementation ({})", reactor_backend_selector::available()))
    , aio_fsync(*this, "aio-fsync", kernel_supports_aio_fsync(),
                "Use Linux aio for fsync() calls. This reduces latency; requires Linux 4.18 or later.")
    , max_networking_io_control_blocks(*this, "max-networking-io-control-blocks", 10000,
                "Maximum number of I/O control blocks (IOCBs) to allocate per shard. This translates to the number of sockets supported per shard."
                " Requires tuning /proc/sys/fs/aio-max-nr. Only valid for the linux-aio reactor backend (see --reactor-backend).")
    , reserve_io_control_blocks(*this, "reserve-io-control-blocks", 0,
                "Reserve this many IOCBs, so it is available to any side application that runs parallel to the seastar appliation."
                " Takes precedence over --max-networking-io-control-blocks. Only valid for the linux-aio reactor backend (see --reactor-backend).")
#ifdef SEASTAR_HEAPPROF
    , heapprof(*this, "heapprof", 0, "Enable seastar heap profiling. Sample every ARG bytes. 0 means off")
#else
    , heapprof(*this, "heapprof", program_options::unused{})
#endif
    , no_handle_interrupt(*this, "no-handle-interrupt", "ignore SIGINT (for gdb)")
{
}

smp_options::smp_options(program_options::option_group* parent_group)
    : program_options::option_group(parent_group, "SMP options")
    , smp(*this, "smp", {}, "number of threads (default: one per CPU)")
    , cpuset(*this, "cpuset", {}, "CPUs to use (in cpuset(7) list format (ex: 0,1-3,7); default: all))")
    , memory(*this, "memory", std::nullopt, "memory to use, in bytes (ex: 4G) (default: all)")
    , reserve_memory(*this, "reserve-memory", {}, "memory reserved to OS (if --memory not specified)")
    , hugepages(*this, "hugepages", {}, "path to accessible hugetlbfs mount (typically /dev/hugepages/something)")
    , lock_memory(*this, "lock-memory", {}, "lock all memory (prevents swapping)")
    , thread_affinity(*this, "thread-affinity", true, "pin threads to their cpus (disable for overprovisioning)")
#ifdef SEASTAR_HAVE_HWLOC
    , num_io_groups(*this, "num-io-groups", {}, "Number of IO groups. Each IO group will be responsible for a fraction of the IO requests. Defaults to the number of NUMA nodes")
#else
    , num_io_groups(*this, "num-io-groups", program_options::unused{})
#endif
    , io_properties_file(*this, "io-properties-file", {}, "path to a YAML file describing the characteristics of the I/O Subsystem")
    , io_properties(*this, "io-properties", {}, "a YAML string describing the characteristics of the I/O Subsystem")
    , mbind(*this, "mbind", true, "enable mbind")
#ifndef SEASTAR_NO_EXCEPTION_HACK
    , enable_glibc_exception_scaling_workaround(*this, "enable-glibc-exception-scaling-workaround", true, "enable workaround for glibc/gcc c++ exception scalablity problem")
#else
    , enable_glibc_exception_scaling_workaround(*this, "enable-glibc-exception-scaling-workaround", program_options::unused{})
#endif
#ifdef SEASTAR_HAVE_HWLOC
    , allow_cpus_in_remote_numa_nodes(*this, "allow-cpus-in-remote-numa-nodes", true, "if some CPUs are found not to have any local NUMA nodes, allow assigning them to remote ones")
#else
    , allow_cpus_in_remote_numa_nodes(*this, "allow-cpus-in-remote-numa-nodes", program_options::unused{})
#endif
{
}

struct reactor_deleter {
    void operator()(reactor* p) {
        p->~reactor();
        free(p);
    }
};

thread_local std::unique_ptr<reactor, reactor_deleter> reactor_holder;

thread_local smp_message_queue** smp::_qs;
thread_local std::thread::id smp::_tmain;
unsigned smp::count = 0;

void smp::start_all_queues()
{
    for (unsigned c = 0; c < count; c++) {
        if (c != this_shard_id()) {
            _qs[c][this_shard_id()].start(c);
        }
    }
    _alien._qs[this_shard_id()].start();
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

void smp::allocate_reactor(unsigned id, reactor_backend_selector rbs, reactor_config cfg) {
    SEASTAR_ASSERT(!reactor_holder);

    // we cannot just write "local_engin = new reactor" since reactor's constructor
    // uses local_engine
    void *buf;
    int r = posix_memalign(&buf, cache_line_size, sizeof(reactor));
    SEASTAR_ASSERT(r == 0);
    *internal::this_shard_id_ptr() = id;
    local_engine = new (buf) reactor(this->shared_from_this(), _alien, id, std::move(rbs), cfg);
    reactor_holder.reset(local_engine);
}

void smp::cleanup() noexcept {
    smp::_threads = std::vector<posix_thread>();
    _thread_loops.clear();
    reactor_holder.reset();
    local_engine = nullptr;
}

void smp::cleanup_cpu() {
    size_t cpuid = this_shard_id();

    if (_qs) {
        for(unsigned i = 0; i < smp::count; i++) {
            _qs[i][cpuid].stop();
        }
    }
    if (_alien._qs) {
        _alien._qs[cpuid].stop();
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
            signal(sig, SIG_DFL);
            Func();
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

static void reraise_signal(int signo) {
    signal(signo, SIG_DFL);
    pthread_kill(pthread_self(), signo);
}

static void sigsegv_action() noexcept {
    print_with_backtrace("Segmentation fault");
    reraise_signal(SIGSEGV);
}

static void sigabrt_action() noexcept {
    print_with_backtrace("Aborting");
    reraise_signal(SIGABRT);
}

// We don't need to handle SIGSEGV when asan is enabled.
#ifdef SEASTAR_ASAN_ENABLED
template<>
void install_oneshot_signal_handler<SIGSEGV, sigsegv_action>() {
    (void)sigsegv_action;
}
#endif

void smp::qs_deleter::operator()(smp_message_queue** qs) const {
    for (unsigned i = 0; i < smp::count; i++) {
        for (unsigned j = 0; j < smp::count; j++) {
            qs[i][j].~smp_message_queue();
        }
        ::operator delete[](qs[i], std::align_val_t(alignof(smp_message_queue))
        );
    }
    delete[](qs);
}

class disk_config_params {
private:
    const unsigned _max_queues;
    unsigned _num_io_groups = 0;
    std::unordered_map<dev_t, mountpoint_params> _mountpoints;
    std::chrono::duration<double> _latency_goal;
    std::chrono::milliseconds _stall_threshold;
    double _flow_ratio_backpressure_threshold;

public:
    explicit disk_config_params(unsigned max_queues) noexcept
            : _max_queues(max_queues)
    {}

    uint64_t per_io_group(uint64_t qty, unsigned nr_groups) const noexcept {
        return std::max(qty / nr_groups, 1ul);
    }

    unsigned num_io_groups() const noexcept { return _num_io_groups; }

    std::chrono::duration<double> latency_goal() const {
        return _latency_goal;
    }

    std::chrono::milliseconds stall_threshold() const {
        return _stall_threshold;
    }

    double latency_goal_opt(const reactor_options& opts) const {
        return opts.io_latency_goal_ms ?
                opts.io_latency_goal_ms.get_value() :
                opts.task_quota_ms.get_value() * 1.5;
    }

    void parse_config(const smp_options& smp_opts, const reactor_options& reactor_opts) {
        seastar_logger.debug("smp::count: {}", smp::count);
        _latency_goal = std::chrono::duration_cast<std::chrono::duration<double>>(latency_goal_opt(reactor_opts) * 1ms);
        seastar_logger.debug("latency_goal: {}", latency_goal().count());
        _flow_ratio_backpressure_threshold = reactor_opts.io_flow_ratio_threshold.get_value();
        seastar_logger.debug("flow-ratio threshold: {}", _flow_ratio_backpressure_threshold);
        _stall_threshold = reactor_opts.io_completion_notify_ms.defaulted() ? std::chrono::milliseconds::max() : reactor_opts.io_completion_notify_ms.get_value() * 1ms;

        if (smp_opts.num_io_groups) {
            _num_io_groups = smp_opts.num_io_groups.get_value();
            if (!_num_io_groups) {
                throw std::runtime_error("num-io-groups must be greater than zero");
            }
        }
        if (smp_opts.io_properties_file && smp_opts.io_properties) {
            throw std::runtime_error("Both io-properties and io-properties-file specified. Don't know which to trust!");
        }

        std::optional<YAML::Node> doc;
        if (smp_opts.io_properties_file) {
            doc = YAML::LoadFile(smp_opts.io_properties_file.get_value());
        } else if (smp_opts.io_properties) {
            doc = YAML::Load(smp_opts.io_properties.get_value());
        }

        if (doc) {
            if (!doc->IsMap()) {
                throw std::runtime_error("Bogus io-properties (did you mix up --io-properties and --io-properties-file?)");
            }
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

                    auto st_dev = S_ISBLK(buf.st_mode) ? buf.st_rdev : buf.st_dev;
                    if (_mountpoints.count(st_dev)) {
                        throw std::runtime_error(fmt::format("Mountpoint {} already configured", d.mountpoint));
                    }
                    if (_mountpoints.size() >= _max_queues) {
                        throw std::runtime_error(fmt::format("Configured number of queues {} is larger than the maximum {}",
                                                 _mountpoints.size(), _max_queues));
                    }

                    d.read_bytes_rate *= d.rate_factor;
                    d.write_bytes_rate *= d.rate_factor;
                    d.read_req_rate *= d.rate_factor;
                    d.write_req_rate *= d.rate_factor;

                    if (d.read_bytes_rate == 0 || d.write_bytes_rate == 0 ||
                            d.read_req_rate == 0 || d.write_req_rate == 0) {
                        throw std::runtime_error(fmt::format("R/W bytes and req rates must not be zero"));
                    }

                    seastar_logger.debug("dev_id: {} mountpoint: {}", st_dev, d.mountpoint);
                    _mountpoints.emplace(st_dev, d);
                }
            }
        }

        // Placeholder for unconfigured disks.
        mountpoint_params d = {};
        _mountpoints.emplace(0, d);
    }

    struct io_queue::config generate_config(dev_t devid, unsigned nr_groups) const {
        seastar_logger.debug("generate_config dev_id: {}", devid);
        const mountpoint_params& p = _mountpoints.at(devid);
        struct io_queue::config cfg;

        cfg.devid = devid;

        if (p.read_bytes_rate != std::numeric_limits<uint64_t>::max()) {
            cfg.blocks_count_rate = (io_queue::read_request_base_count * (unsigned long)per_io_group(p.read_bytes_rate, nr_groups)) >> io_queue::block_size_shift;
            cfg.disk_blocks_write_to_read_multiplier = (io_queue::read_request_base_count * p.read_bytes_rate) / p.write_bytes_rate;
        }
        if (p.read_req_rate != std::numeric_limits<uint64_t>::max()) {
            cfg.req_count_rate = io_queue::read_request_base_count * (unsigned long)per_io_group(p.read_req_rate, nr_groups);
            cfg.disk_req_write_to_read_multiplier = (io_queue::read_request_base_count * p.read_req_rate) / p.write_req_rate;
        }
        if (p.read_saturation_length != std::numeric_limits<uint64_t>::max()) {
            cfg.disk_read_saturation_length = p.read_saturation_length;
        }
        if (p.write_saturation_length != std::numeric_limits<uint64_t>::max()) {
            cfg.disk_write_saturation_length = p.write_saturation_length;
        }
        cfg.mountpoint = p.mountpoint;
        cfg.duplex = p.duplex;
        cfg.rate_limit_duration = latency_goal();
        cfg.flow_ratio_backpressure_threshold = _flow_ratio_backpressure_threshold;
        // Block count limit should not be less than the minimal IO size on the device
        // On the other hand, even this is not good enough -- in the worst case the
        // scheduler will self-tune to allow for the single 64k request, while it would
        // be better to sacrifice some IO latency, but allow for larger concurrency
        cfg.block_count_limit_min = (64 << 10) >> io_queue::block_size_shift;
        cfg.stall_threshold = stall_threshold();

        return cfg;
    }

    auto device_ids() {
        return boost::adaptors::keys(_mountpoints);
    }
};

void smp::log_aiocbs(log_level level, unsigned storage, unsigned preempt, unsigned network, unsigned reserve) {
    // Each cell in the table should be
    // - as wide as the grand total,
    // - as wide as its containing column's header,
    // whichever is wider.
    std::string percpu_hdr = format("per cpu");
    std::string allcpus_hdr = format("all {} cpus", smp::count);
    unsigned percpu_total = storage + preempt + network;
    unsigned allcpus_total = reserve + percpu_total * smp::count;
    size_t num_width = format("{}", allcpus_total).length();
    size_t percpu_width = std::max(num_width, percpu_hdr.length());
    size_t allcpus_width = std::max(num_width, allcpus_hdr.length());

    seastar_logger.log(level, "purpose  {:{}}  {:{}}",     percpu_hdr,   percpu_width, allcpus_hdr,          allcpus_width);
    seastar_logger.log(level, "-------  {:-<{}}  {:-<{}}", "",           percpu_width, "",                   allcpus_width);
    seastar_logger.log(level, "reserve  {:{}}  {:{}}",     "",           percpu_width, reserve,              allcpus_width);
    seastar_logger.log(level, "storage  {:{}}  {:{}}",     storage,      percpu_width, storage * smp::count, allcpus_width);
    seastar_logger.log(level, "preempt  {:{}}  {:{}}",     preempt,      percpu_width, preempt * smp::count, allcpus_width);
    seastar_logger.log(level, "network  {:{}}  {:{}}",     network,      percpu_width, network * smp::count, allcpus_width);
    seastar_logger.log(level, "-------  {:-<{}}  {:-<{}}", "",           percpu_width, "",                   allcpus_width);
    seastar_logger.log(level, "total    {:{}}  {:{}}",     percpu_total, percpu_width, allcpus_total,        allcpus_width);
}

unsigned smp::adjust_max_networking_aio_io_control_blocks(unsigned network_iocbs, unsigned reserve_iocbs)
{
    static unsigned constexpr storage_iocbs = reactor::max_aio;
    static unsigned constexpr preempt_iocbs = 2;

    auto aio_max_nr = read_first_line_as<unsigned>("/proc/sys/fs/aio-max-nr");
    auto aio_nr = read_first_line_as<unsigned>("/proc/sys/fs/aio-nr");
    auto available_aio = aio_max_nr - aio_nr;
    auto requested_aio_network = network_iocbs * smp::count;
    auto requested_aio_other = reserve_iocbs + (storage_iocbs + preempt_iocbs) * smp::count;
    auto requested_aio = requested_aio_network + requested_aio_other;

    seastar_logger.debug("Intended AIO control block usage:");
    seastar_logger.debug("");
    log_aiocbs(log_level::debug, storage_iocbs, preempt_iocbs, network_iocbs, reserve_iocbs);
    seastar_logger.debug("");
    seastar_logger.debug("Available AIO control blocks = aio-max-nr - aio-nr = {} - {} = {}", aio_max_nr, aio_nr, available_aio);

    if (available_aio < requested_aio) {
        if (available_aio >= requested_aio_other + smp::count) { // at least one queue for each shard
            network_iocbs = (available_aio - requested_aio_other) / smp::count;
            seastar_logger.warn("Your system does not have enough AIO capacity for optimal network performance; reducing `max-networking-io-control-blocks'.");
            seastar_logger.warn("Resultant AIO control block usage:");
            seastar_logger.warn("");
            log_aiocbs(log_level::warn, storage_iocbs, preempt_iocbs, network_iocbs, reserve_iocbs);
            seastar_logger.warn("");
            seastar_logger.warn("For optimal network performance, set /proc/sys/fs/aio-max-nr to at least {}.", aio_nr + requested_aio);
        } else {
            throw std::runtime_error(
                format("Your system does not satisfy minimum AIO requirements. "
                       "Set /proc/sys/fs/aio-max-nr to at least {} (minimum) or {} (recommended for networking performance).",
                       aio_nr + (requested_aio_other + smp::count), aio_nr + requested_aio));
        }
    }

    return network_iocbs;
}

void smp::configure(const smp_options& smp_opts, const reactor_options& reactor_opts)
{
    bool use_transparent_hugepages = !reactor_opts.overprovisioned;

#ifndef SEASTAR_NO_EXCEPTION_HACK
    if (smp_opts.enable_glibc_exception_scaling_workaround.get_value()) {
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
    if (!reactor_opts._auto_handle_sigint_sigterm) {
        sigdelset(&sigs, SIGINT);
        sigdelset(&sigs, SIGTERM);
    }
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

    install_oneshot_signal_handler<SIGSEGV, sigsegv_action>();
    install_oneshot_signal_handler<SIGABRT, sigabrt_action>();

#ifdef SEASTAR_HAVE_DPDK
    const auto* native_stack = dynamic_cast<const net::native_stack_options*>(reactor_opts.network_stack.get_selected_candidate_opts());
    _using_dpdk = native_stack && native_stack->dpdk_pmd;
#endif
    auto thread_affinity = smp_opts.thread_affinity.get_value();
    if (reactor_opts.overprovisioned
           && smp_opts.thread_affinity.defaulted()) {
        thread_affinity = false;
    }
    if (!thread_affinity && _using_dpdk) {
        fmt::print("warning: --thread-affinity 0 ignored in dpdk mode\n");
    }
    auto mbind = smp_opts.mbind.get_value();
    if (!thread_affinity) {
        mbind = false;
    }

    resource::configuration rc;

    smp::_tmain = std::this_thread::get_id();
    resource::cpuset cpu_set = get_current_cpuset();

    if (smp_opts.cpuset) {
        auto opts_cpuset = smp_opts.cpuset.get_value();
        // CPUs that are not available are those pinned by
        // --cpuset but not present in current task set
        std::set<unsigned int> not_available_cpus;
        std::set_difference(opts_cpuset.begin(), opts_cpuset.end(),
                            cpu_set.begin(), cpu_set.end(),
                            std::inserter(not_available_cpus, not_available_cpus.end()));

        if (!not_available_cpus.empty()) {
            std::ostringstream not_available_cpus_list;
            for (auto cpu_id : not_available_cpus) {
                not_available_cpus_list << " " << cpu_id;
            }
            seastar_logger.error("Bad value for --cpuset:{} not allowed. Shutting down.", not_available_cpus_list.str());
            exit(1);
        }
        cpu_set = opts_cpuset;
    }

    if (smp_opts.smp) {
        smp::count = smp_opts.smp.get_value();
    } else {
        smp::count = cpu_set.size();
    }
    std::vector<reactor*> reactors(smp::count);
    if (smp_opts.memory) {
#ifdef SEASTAR_DEFAULT_ALLOCATOR
        seastar_logger.warn("Seastar compiled with default allocator, --memory option won't take effect");
#endif
        rc.total_memory = parse_memory_size(smp_opts.memory.get_value());
#ifdef SEASTAR_HAVE_DPDK
        if (smp_opts.hugepages &&
            !reactor_opts.network_stack.get_selected_candidate_name().compare("native") &&
            _using_dpdk) {
            size_t dpdk_memory = dpdk::eal::mem_size(smp::count);

            if (dpdk_memory >= rc.total_memory) {
                seastar_logger.error("Can't run with the given amount of memory: {}. "
                                     "Consider giving more.",
                                     smp_opts.memory.get_value());
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
    if (smp_opts.reserve_memory) {
        rc.reserve_memory = parse_memory_size(smp_opts.reserve_memory.get_value());
    }
    rc.reserve_additional_memory_per_shard = smp_opts.reserve_additional_memory_per_shard;
    std::optional<std::string> hugepages_path;
    if (smp_opts.hugepages) {
        hugepages_path = smp_opts.hugepages.get_value();
    }
    auto mlock = false;
    if (smp_opts.lock_memory) {
        mlock = smp_opts.lock_memory.get_value();
    }
    if (mlock) {
        auto extra_flags = 0;
#ifdef MCL_ONFAULT
        // Linux will serialize faulting in anonymous memory, and also
        // serialize marking them as locked. This can take many minutes on
        // terabyte class machines, so fault them in the future to spread
        // out the cost. This isn't good since we'll see contention if
        // multiple shards fault in memory at once, but if that work can be
        // in parallel to regular reactor work on other shards.
        extra_flags |= MCL_ONFAULT; // Linux 4.4+
#endif
        auto r = mlockall(MCL_CURRENT | MCL_FUTURE | extra_flags);
        if (r) {
            // Don't hard fail for now, it's hard to get the configuration right
            fmt::print("warning: failed to mlockall: {}\n", strerror(errno));
        }
    }

    rc.cpus = smp::count;
    rc.cpu_set = std::move(cpu_set);

    disk_config_params disk_config(reactor::max_queues);
    disk_config.parse_config(smp_opts, reactor_opts);
    for (auto& id : disk_config.device_ids()) {
        rc.devices.push_back(id);
    }
    rc.num_io_groups = disk_config.num_io_groups();

#ifdef SEASTAR_HAVE_HWLOC
    if (smp_opts.allow_cpus_in_remote_numa_nodes.get_value()) {
        rc.assign_orphan_cpus = true;
    }
#endif

    auto resources = resource::allocate(rc);
    logger::set_shard_field_width(std::ceil(std::log10(smp::count)));
    std::vector<resource::cpu> allocations = std::move(resources.cpus);
    if (thread_affinity) {
        smp::pin(allocations[0].cpu_id);
    }
    std::optional<memory::internal::numa_layout> layout;
    if (smp_opts.memory_allocator == memory_allocator::seastar) {
        layout = memory::configure(allocations[0].mem, mbind, use_transparent_hugepages, hugepages_path);
    } else {
        // #2148 - if running seastar allocator but options that contradict this, we still need to
        // init memory at least minimally, otherwise a bunch of stuff breaks.
        // Previously, we got away wth this by accident due to #2137.
        memory::configure_minimal();
    }

    if (reactor_opts.abort_on_seastar_bad_alloc) {
        memory::set_abort_on_allocation_failure(true);
    }

    if (reactor_opts.dump_memory_diagnostics_on_alloc_failure_kind) {
        memory::set_dump_memory_diagnostics_on_alloc_failure_kind(reactor_opts.dump_memory_diagnostics_on_alloc_failure_kind.get_value());
    }

    auto max_networking_aio_io_control_blocks = reactor_opts.max_networking_io_control_blocks.get_value();
    // Prevent errors about insufficient AIO blocks, when they are not needed by the reactor backend.
    if (reactor_opts.reactor_backend.get_selected_candidate().name() == "linux-aio") {
        max_networking_aio_io_control_blocks = adjust_max_networking_aio_io_control_blocks(max_networking_aio_io_control_blocks,
                reactor_opts.reserve_io_control_blocks.get_value());
    }

    reactor_config reactor_cfg = {
        .task_quota = std::chrono::duration_cast<sched_clock::duration>(reactor_opts.task_quota_ms.get_value() * 1ms),
        .max_poll_time = [&reactor_opts] () -> std::chrono::nanoseconds {
            if (reactor_opts.poll_mode) {
                return std::chrono::nanoseconds::max();
            } else if (reactor_opts.overprovisioned && reactor_opts.idle_poll_time_us.defaulted()) {
                return 0us;
            } else {
                return reactor_opts.idle_poll_time_us.get_value() * 1us;
            }
        }(),
        .handle_sigint = !reactor_opts.no_handle_interrupt,
        .auto_handle_sigint_sigterm = reactor_opts._auto_handle_sigint_sigterm,
        .max_networking_aio_io_control_blocks = max_networking_aio_io_control_blocks,
        .force_io_getevents_syscall = reactor_opts.force_aio_syscalls.get_value(),
        .kernel_page_cache = reactor_opts.kernel_page_cache.get_value(),
        .have_aio_fsync = reactor_opts.aio_fsync.get_value(),
        .max_task_backlog = reactor_opts.max_task_backlog.get_value(),
        .strict_o_direct = !reactor_opts.relaxed_dma,
        .bypass_fsync = reactor_opts.unsafe_bypass_fsync.get_value(),
        .no_poll_aio = !reactor_opts.poll_aio.get_value() || (reactor_opts.poll_aio.defaulted() && reactor_opts.overprovisioned),
    };

    aio_nowait_supported = reactor_opts.linux_aio_nowait.get_value();
    std::mutex mtx;

#ifdef SEASTAR_HEAPPROF
    size_t heapprof_sampling_rate = reactor_opts.heapprof.get_value();
    if (heapprof_sampling_rate) {
        memory::set_heap_profiling_sampling_rate(heapprof_sampling_rate);
    }
#else
    size_t heapprof_sampling_rate = 0;
#endif

#ifdef SEASTAR_HAVE_DPDK
    if (_using_dpdk) {
        dpdk::eal::cpuset cpus;
        for (auto&& a : allocations) {
            cpus[a.cpu_id] = true;
        }
        dpdk::eal::init(cpus, reactor_opts._argv0, hugepages_path, native_stack ? bool(native_stack->dpdk_pmd) : false);
    }
#endif

    // Better to put it into the smp class, but at smp construction time
    // correct smp::count is not known.
    boost::barrier reactors_registered(smp::count);
    boost::barrier smp_queues_constructed(smp::count);
    // We use shared_ptr since this thread can exit while other threads are still unlocking
    auto inited = std::make_shared<boost::barrier>(smp::count);

    auto ioq_topology = std::move(resources.ioq_topology);

    // ATTN: The ioq_topology value is referenced by below lambdas which are
    // then copied to other shard's threads, so each shard has a copy of the
    // ioq_topology on stack, but (!) still references and uses the value
    // from shard-0. This access is race-free because
    //  1. The .shard_to_group is not modified
    //  2. The .queues is pre-resize()-d in advance, so the vector itself
    //     doesn't change; existing slots are accessed by owning shards only
    //     without interference
    //  3. The .groups manipulations are guarded by the .lock lock (but it's
    //     also pre-resize()-d in advance)

    auto alloc_io_queues = [&ioq_topology, &disk_config] (shard_id shard) {
        for (auto& [dev, io_info] : ioq_topology) {
            auto group_idx = io_info.shard_to_group[shard];
            std::shared_ptr<io_group> group;

            {
                std::lock_guard _(io_info.lock);
                auto& iog = io_info.groups[group_idx];
                if (!iog) {
                    struct io_queue::config qcfg = disk_config.generate_config(dev, io_info.groups.size());
                    iog = std::make_shared<io_group>(std::move(qcfg), io_info.shards_in_group[group_idx]);
                    seastar_logger.debug("allocate {} IO group with {} queues, dev {}", group_idx, io_info.shards_in_group[group_idx], dev);
                }
                group = iog;
            }

            io_info.queues[shard] = std::make_unique<io_queue>(std::move(group), engine()._io_sink);
            seastar_logger.debug("attached {} queue to {} IO group, dev {}", shard, group_idx, dev);
        }
    };

    auto assign_io_queues = [&ioq_topology] (shard_id shard) {
        for (auto& [dev, io_info] : ioq_topology) {
            auto queue = std::move(io_info.queues[shard]);
            SEASTAR_ASSERT(queue);
            engine()._io_queues.emplace(dev, std::move(queue));

            auto num_io_groups = io_info.groups.size();
            if (engine()._num_io_groups == 0) {
                engine()._num_io_groups = num_io_groups;
            } else if (engine()._num_io_groups != num_io_groups) {
                throw std::logic_error(format("Number of IO-groups mismatch, {} != {}", engine()._num_io_groups, num_io_groups));
            }
        }
    };

    _all_event_loops_done.emplace(smp::count);

    auto backend_selector = reactor_opts.reactor_backend.get_selected_candidate();
    seastar_logger.info("Reactor backend: {}", backend_selector);

    unsigned i;
    auto smp_tmain = smp::_tmain;
    for (i = 1; i < smp::count; i++) {
        auto allocation = allocations[i];
        create_thread([this, smp_tmain, inited, &reactors_registered, &smp_queues_constructed, &smp_opts, &reactor_opts, &reactors, hugepages_path, i, allocation, assign_io_queues, alloc_io_queues, thread_affinity, heapprof_sampling_rate, mbind, backend_selector, reactor_cfg, &mtx, &layout, use_transparent_hugepages] {
          try {
            // initialize thread_locals that are equal across all reacto threads of this smp instance
            smp::_tmain = smp_tmain;
            auto thread_name = fmt::format("reactor-{}", i);
            pthread_setname_np(pthread_self(), thread_name.c_str());
            if (thread_affinity) {
                smp::pin(allocation.cpu_id);
            }
            if (smp_opts.memory_allocator == memory_allocator::seastar) {
                auto another_layout = memory::configure(allocation.mem, mbind, use_transparent_hugepages, hugepages_path);
                auto guard = std::lock_guard(mtx);
                *layout = memory::internal::merge(std::move(*layout), std::move(another_layout));
            } else {
                // See comment above (shard 0)
                memory::configure_minimal();
            }
            if (heapprof_sampling_rate) {
                memory::set_heap_profiling_sampling_rate(heapprof_sampling_rate);
            }
            sigset_t mask;
            sigfillset(&mask);
            for (auto sig : { SIGSEGV }) {
                sigdelset(&mask, sig);
            }
            auto r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
            throw_pthread_error(r);
            init_default_smp_service_group(i);
            lowres_clock::update();
            allocate_reactor(i, backend_selector, reactor_cfg);
            reactors[i] = &engine();
            alloc_io_queues(i);
            reactors_registered.wait();
            smp_queues_constructed.wait();
            // _qs_owner is only initialized here
            _qs = _qs_owner.get();
            start_all_queues();
            assign_io_queues(i);
            inited->wait();
            engine().configure(reactor_opts);
            engine().do_run();
          } catch (const std::exception& e) {
              seastar_logger.error("{}", e.what());
              _exit(1);
          }
        });
    }

    init_default_smp_service_group(0);
    lowres_clock::update();
    try {
        allocate_reactor(0, backend_selector, reactor_cfg);
    } catch (const std::exception& e) {
        seastar_logger.error("{}", e.what());
        _exit(1);
    }

    reactors[0] = &engine();
    alloc_io_queues(0);

#ifdef SEASTAR_HAVE_DPDK
    if (_using_dpdk) {
        auto it = _thread_loops.begin();
        RTE_LCORE_FOREACH_WORKER(i) {
            rte_eal_remote_launch(dpdk_thread_adaptor, static_cast<void*>(&*(it++)), i);
        }
    }
#endif

    reactors_registered.wait();
    _qs_owner = decltype(smp::_qs_owner){new smp_message_queue* [smp::count], qs_deleter{}};
    _qs = _qs_owner.get();
    for(unsigned i = 0; i < smp::count; i++) {
        smp::_qs_owner[i] = reinterpret_cast<smp_message_queue*>(operator new[] (sizeof(smp_message_queue) * smp::count
        // smp_message_queue has members with hefty alignment requirements.
        // if we are reactor thread, or not running with dpdk, doing this
        // new default aligned seemingly works, as does reordering
        // dlinit dependencies (ugh). But we should enforce calling out to
        // aligned_alloc, instead of pure malloc, if possible.
            , std::align_val_t(alignof(smp_message_queue))
        ));
        for (unsigned j = 0; j < smp::count; ++j) {
            new (&smp::_qs_owner[i][j]) smp_message_queue(reactors[j], reactors[i]);
        }
    }
    _alien._qs = alien::instance::create_qs(reactors);
    smp_queues_constructed.wait();
    start_all_queues();
    assign_io_queues(0);
    inited->wait();

    engine().configure(reactor_opts);

    if (smp_opts.lock_memory && smp_opts.lock_memory.get_value() && layout && !layout->ranges.empty()) {
        smp::setup_prefaulter(resources, std::move(*layout));
    }
}

bool smp::poll_queues() {
    size_t got = 0;
    for (unsigned i = 0; i < count; i++) {
        if (this_shard_id() != i) {
            auto& rxq = _qs[this_shard_id()][i];
            rxq.flush_response_batch();
            got += rxq.has_unflushed_responses();
            got += rxq.process_incoming();
            auto& txq = _qs[i][this_shard_id()];
            txq.flush_request_batch();
            got += txq.process_completions(i);
        }
    }
    return got != 0;
}

bool smp::pure_poll_queues() {
    for (unsigned i = 0; i < count; i++) {
        if (this_shard_id() != i) {
            auto& rxq = _qs[this_shard_id()][i];
            rxq.flush_response_batch();
            auto& txq = _qs[i][this_shard_id()];
            txq.flush_request_batch();
            if (rxq.pure_poll_rx() || txq.pure_poll_tx() || rxq.has_unflushed_responses()) {
                return true;
            }
        }
    }
    return false;
}

__thread reactor* local_engine;

void report_exception(std::string_view message, std::exception_ptr eptr) noexcept {
    seastar_logger.error("{}: {}", message, eptr);
}

future<> check_direct_io_support(std::string_view path) noexcept {
    struct w {
        sstring path;
        open_flags flags;
        std::function<future<>()> cleanup;

        static w parse(sstring path, std::optional<directory_entry_type> type) {
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

    // Allocating memory for a sstring can throw, hence the futurize_invoke
    return futurize_invoke([path] {
        return engine().file_type(path).then([path = sstring(path)] (auto type) {
            auto w = w::parse(path, type);
            return open_file_dma(w.path, w.flags).then_wrapped([path = w.path, cleanup = std::move(w.cleanup)] (future<file> f) {
                try {
                    auto fd = f.get();
                    return cleanup().finally([fd = std::move(fd)] () mutable {
                        return fd.close();
                    });
                } catch (std::system_error& e) {
                    if (e.code() == std::error_code(EINVAL, std::system_category())) {
                        report_exception(format("Could not open file at {}. Does your filesystem support O_DIRECT?", path), std::current_exception());
                    }
                    throw;
                }
            });
        });
    });
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

socket make_socket() {
    return engine().net().socket();
}

net::udp_channel make_udp_channel() {
    return make_unbound_datagram_channel(AF_INET);
}

net::udp_channel make_udp_channel(const socket_address& local) {
    return make_bound_datagram_channel(local);
}

net::datagram_channel make_unbound_datagram_channel(sa_family_t family) {
    return engine().net().make_unbound_datagram_channel(family);
}

net::datagram_channel make_bound_datagram_channel(const socket_address& local) {
    return engine().net().make_bound_datagram_channel(local);
}

void reactor::add_high_priority_task(task* t) noexcept {
    add_urgent_task(t);
    // break .then() chains
    request_preemption();
}


void set_idle_cpu_handler(idle_cpu_handler&& handler) {
    engine().set_idle_cpu_handler(std::move(handler));
}

namespace experimental {
future<std::tuple<file_desc, file_desc>> make_pipe() {
    return engine().make_pipe();
}

future<process> spawn_process(const std::filesystem::path& pathname,
                              spawn_parameters params) {
    return process::spawn(pathname, std::move(params));
}

future<process> spawn_process(const std::filesystem::path& pathname) {
    return process::spawn(pathname);
}
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

future<>
yield() noexcept {
    memory::scoped_critical_alloc_section _;
    auto tsk = make_task([] {});
    schedule(tsk);
    return tsk->get_future();
}

future<> check_for_io_immediately() noexcept {
    memory::scoped_critical_alloc_section _;
    engine().force_poll();
    auto tsk = make_task(default_scheduling_group(), [] {});
    schedule(tsk);
    return tsk->get_future();
}

future<> later() noexcept {
    return check_for_io_immediately();
}

steady_clock_type::duration reactor::total_idle_time() {
    return _total_idle;
}

steady_clock_type::duration reactor::total_busy_time() {
    return now() - _start_time - _total_idle;
}

steady_clock_type::duration reactor::total_awake_time() const {
    return now() - _start_time - _total_sleep;
}

std::chrono::nanoseconds reactor::total_cpu_time() const {
    return thread_cputime_clock::now().time_since_epoch();
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
    // Furthermore, not all steal is from other processes: time used by the syscall thread and any
    // alien threads will show up as steal as well as any time spent in a system call that
    // unexpectedly blocked (since CPU time won't tick up when that occurs).
    //
    // But what we have here should be good enough and at least has a well defined meaning.
    //
    // Because we calculate sleep time with timestamps around polling methods that may sleep, like
    // io_getevents, we systematically over-count sleep time, since there is CPU usage within the
    // period timed as sleep, before and after an actual sleep occurs (and no sleep may occur at all,
    // e.g., if there are events immediately available). Over-counting sleep means we under-count the
    // wall-clock awake time, and so if there is no "true" steal, we will generally have a small
    // *negative* steal time, because we under-count awake wall clock time while thread CPU time does
    // not have a corresponding error.
    //
    // Becuase we claim "steal" is a counter, we must ensure that it never deceases, because PromQL
    // functions which use counters will produce non-sensical results if they do. Therefore we clamp
    // the output such that it never decreases.
    //
    // Finally, we don't just clamp difference of awake and CPU time since proces start at 0, but
    // take the last value we returned from this function and then calculate the incremental steal
    // time since that measurement, clamped to 0. This means that as soon as steal time becomes
    // positive, it will be reflected in the measurement, rather than needing to "consume" all the
    // accumulated negative steal time before positive steal times start showing up.


    auto true_steal = total_awake_time() - total_cpu_time();
    auto mono_steal = _last_mono_steal + std::max(true_steal - _last_true_steal, 0ns);

    _last_true_steal = true_steal;
    _last_mono_steal = mono_steal;

    return mono_steal;
}

static std::atomic<unsigned long> s_used_scheduling_group_ids_bitmap{3}; // 0=main, 1=atexit
static std::atomic<unsigned long> s_next_scheduling_group_specific_key{0};

static
int
allocate_scheduling_group_id() noexcept {
    static_assert(max_scheduling_groups() <= std::numeric_limits<unsigned long>::digits, "more scheduling groups than available bits");
    auto b = s_used_scheduling_group_ids_bitmap.load(std::memory_order_relaxed);
    auto nb = b;
    unsigned i = 0;
    do {
        if (__builtin_popcountl(b) == max_scheduling_groups()) {
            return -1;
        }
        i = count_trailing_zeros(~b);
        nb = b | (1ul << i);
    } while (!s_used_scheduling_group_ids_bitmap.compare_exchange_weak(b, nb, std::memory_order_relaxed));
    return i;
}

static
unsigned long
allocate_scheduling_group_specific_key() noexcept {
    return  s_next_scheduling_group_specific_key.fetch_add(1, std::memory_order_relaxed);
}

static
void
deallocate_scheduling_group_id(unsigned id) noexcept {
    s_used_scheduling_group_ids_bitmap.fetch_and(~(1ul << id), std::memory_order_relaxed);
}

static
internal::scheduling_group_specific_thread_local_data::specific_val
allocate_scheduling_group_specific_data(scheduling_group sg, unsigned long key_id, const lw_shared_ptr<scheduling_group_key_config>& cfg) {
    using val_ptr = internal::scheduling_group_specific_thread_local_data::val_ptr;
    using specific_val = internal::scheduling_group_specific_thread_local_data::specific_val;

    val_ptr valp(aligned_alloc(cfg->alignment, cfg->allocation_size), &free);
    if (!valp) {
        throw std::runtime_error("memory allocation failed");
    }
    return specific_val(std::move(valp), cfg);
}

future<>
reactor::rename_scheduling_group_specific_data(scheduling_group sg) {
    return with_shared(_scheduling_group_keys_mutex, [this, sg] {
        return with_scheduling_group(sg, [this, sg] {
            get_sg_data(sg).rename();
        });
    });
}

future<>
reactor::init_scheduling_group(seastar::scheduling_group sg, sstring name, sstring shortname, float shares) {
    return with_shared(_scheduling_group_keys_mutex, [this, sg, name = std::move(name), shortname = std::move(shortname), shares] {
        get_sg_data(sg).queue_is_initialized = true;
        _task_queues.resize(std::max<size_t>(_task_queues.size(), sg._id + 1));
        _task_queues[sg._id] = std::make_unique<task_queue>(sg._id, name, shortname, shares);

        return with_scheduling_group(sg, [this, sg] () {
            auto& sg_data = _scheduling_group_specific_data;
            auto& this_sg = get_sg_data(sg);
            for (const auto& [key_id, cfg] : sg_data.scheduling_group_key_configs) {
                this_sg.specific_vals.resize(std::max<size_t>(this_sg.specific_vals.size(), key_id+1));
                this_sg.specific_vals[key_id] = allocate_scheduling_group_specific_data(sg, key_id, cfg);
            }
        });
    });
}

future<>
reactor::init_new_scheduling_group_key(scheduling_group_key key, scheduling_group_key_config cfg) {
    return with_lock(_scheduling_group_keys_mutex, [this, key, cfg] {
        auto key_id = internal::scheduling_group_key_id(key);
        auto cfgp = make_lw_shared<scheduling_group_key_config>(std::move(cfg));
        _scheduling_group_specific_data.scheduling_group_key_configs[key_id] = cfgp;
        return parallel_for_each(_task_queues, [this, cfgp, key_id] (std::unique_ptr<task_queue>& tq) {
            if (tq) {
                scheduling_group sg = scheduling_group(tq->_id);
                if (tq.get() == _at_destroy_tasks) {
                    // fake the group by assuming it here
                    auto curr = current_scheduling_group();
                    auto cleanup = defer([curr] () noexcept { *internal::current_scheduling_group_ptr() = curr; });
                    *internal::current_scheduling_group_ptr() = sg;
                    auto& this_sg = get_sg_data(sg);
                    this_sg.specific_vals.resize(std::max<size_t>(this_sg.specific_vals.size(), key_id+1));
                    this_sg.specific_vals[key_id] = allocate_scheduling_group_specific_data(sg, key_id, cfgp);
                } else {
                    return with_scheduling_group(sg, [this, key_id, sg, cfgp = std::move(cfgp)] () {
                        auto& this_sg = get_sg_data(sg);
                        this_sg.specific_vals.resize(std::max<size_t>(this_sg.specific_vals.size(), key_id+1));
                        this_sg.specific_vals[key_id] = allocate_scheduling_group_specific_data(sg, key_id, cfgp);
                    });
                }
            }
            return make_ready_future();
        });
    });
}

future<>
reactor::destroy_scheduling_group(scheduling_group sg) noexcept {
    if (sg._id >= max_scheduling_groups()) {
        on_fatal_internal_error(seastar_logger, format("Invalid scheduling_group {}", sg._id));
    }
    return with_scheduling_group(sg, [this, sg] () {
        get_sg_data(sg).specific_vals.clear();
    }).then( [this, sg] () {
        get_sg_data(sg).queue_is_initialized = false;
        _task_queues[sg._id].reset();
    });

}

#ifdef SEASTAR_BUILD_SHARED_LIBS
namespace internal {

scheduling_group_specific_thread_local_data** get_scheduling_group_specific_thread_local_data_ptr() noexcept {
    static thread_local scheduling_group_specific_thread_local_data* data;
    return &data;
}

}
#endif

void
internal::no_such_scheduling_group(scheduling_group sg) {
    throw std::invalid_argument(format("The scheduling group does not exist ({})", internal::scheduling_group_index(sg)));
}

#ifdef SEASTAR_BUILD_SHARED_LIBS
scheduling_group*
internal::current_scheduling_group_ptr() noexcept {
    // Slow unless constructor is constexpr
    static thread_local scheduling_group sg;
    return &sg;
}
#endif

const sstring&
scheduling_group::name() const noexcept {
    return engine()._task_queues[_id]->_name;
}

const sstring&
scheduling_group::short_name() const noexcept {
    if (!engine()._task_queues.empty()) {
        auto& task_queue = engine()._task_queues[_id];
        return task_queue->_shortname;
    }
    // we might want to print logging messages before task_queues are ready.
    // pad this string so its length is task_queue->shortname_size
    static const sstring not_available("n/a ");
    return not_available;
}


float scheduling_group::get_shares() const noexcept {
    return engine()._task_queues[_id]->_shares;
}

void
scheduling_group::set_shares(float shares) noexcept {
    engine()._task_queues[_id]->set_shares(shares);
    engine().update_shares_for_queues(internal::priority_class(*this), shares);
}

future<> scheduling_group::update_io_bandwidth(uint64_t bandwidth) const {
    return engine().update_bandwidth_for_queues(internal::priority_class(*this), bandwidth);
}

future<scheduling_group>
create_scheduling_group(sstring name, sstring shortname, float shares) noexcept {
    auto aid = allocate_scheduling_group_id();
    if (aid < 0) {
        return make_exception_future<scheduling_group>(std::runtime_error(fmt::format("Scheduling group limit exceeded while creating {}", name)));
    }
    auto id = static_cast<unsigned>(aid);
    SEASTAR_ASSERT(id < max_scheduling_groups());
    auto sg = scheduling_group(id);
    return smp::invoke_on_all([sg, name, shortname, shares] {
        return engine().init_scheduling_group(sg, name, shortname, shares);
    }).then([sg] {
        return make_ready_future<scheduling_group>(sg);
    });
}

future<scheduling_group>
create_scheduling_group(sstring name, float shares) noexcept {
    return create_scheduling_group(name, {}, shares);
}

future<scheduling_group_key>
scheduling_group_key_create(scheduling_group_key_config cfg) noexcept {
    scheduling_group_key key = allocate_scheduling_group_specific_key();
    return smp::invoke_on_all([key, cfg] {
        return engine().init_new_scheduling_group_key(key, cfg);
    }).then([key] {
        return make_ready_future<scheduling_group_key>(key);
    });
}

future<>
destroy_scheduling_group(scheduling_group sg) noexcept {
    if (sg == default_scheduling_group()) {
        return make_exception_future<>(make_backtraced_exception_ptr<std::runtime_error>("Attempt to destroy the default scheduling group"));
    }
    if (sg == current_scheduling_group()) {
        return make_exception_future<>(make_backtraced_exception_ptr<std::runtime_error>("Attempt to destroy the current scheduling group"));
    }
    return smp::invoke_on_all([sg] {
        return engine().destroy_scheduling_group(sg);
    }).then([sg] {
        deallocate_scheduling_group_id(sg._id);
    });
}

future<>
rename_scheduling_group(scheduling_group sg, sstring new_name) noexcept {
    return rename_scheduling_group(sg, new_name, {});
}

future<>
rename_scheduling_group(scheduling_group sg, sstring new_name, sstring new_shortname) noexcept {
    if (sg == default_scheduling_group()) {
        return make_exception_future<>(make_backtraced_exception_ptr<std::runtime_error>("Attempt to rename the default scheduling group"));
    }
    return smp::invoke_on_all([sg, new_name, new_shortname] {
        engine()._task_queues[sg._id]->rename(new_name, new_shortname);
        engine().rename_queues(internal::priority_class(sg), new_name);
        return engine().rename_scheduling_group_specific_data(sg);
    });
}

namespace internal {

void add_to_flush_poller(output_stream<char>& os) noexcept {
    engine()._flush_batching.push_back(os);
}

inline
sched_clock::duration
timeval_to_duration(::timeval tv) {
    return std::chrono::seconds(tv.tv_sec) + std::chrono::microseconds(tv.tv_usec);
}

class reactor_stall_sampler : public reactor::pollfn {
    sched_clock::time_point _run_start;
    ::rusage _run_start_rusage;
    uint64_t _kernel_stalls = 0;
    sched_clock::duration _nonsleep_cpu_time = {};
    sched_clock::duration _nonsleep_wall_time = {};
private:
    static ::rusage get_rusage() {
        struct ::rusage ru;
        ::getrusage(RUSAGE_THREAD, &ru);
        return ru;
    }
    static sched_clock::duration cpu_time(const ::rusage& ru) {
        return timeval_to_duration(ru.ru_stime) + timeval_to_duration(ru.ru_utime);
    }
    void mark_run_start() {
        _run_start = reactor::now();
        _run_start_rusage = get_rusage();
    }
    void mark_run_end() {
        auto start_nvcsw = _run_start_rusage.ru_nvcsw;
        auto start_cpu_time = cpu_time(_run_start_rusage);
        auto start_time = _run_start;
        _run_start = reactor::now();
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
    auto to_ms = [] (sched_clock::duration d) -> float {
        return std::chrono::duration<float>(d) / 1ms;
    };
    return os << format("{} stalls, {} ms stall time, {} ms run time", sr.kernel_stalls, to_ms(sr.stall_time), to_ms(sr.run_wall_time));
}

size_t scheduling_group_count() {
    auto b = s_used_scheduling_group_ids_bitmap.load(std::memory_order_relaxed);
    return __builtin_popcountl(b);
}

void
run_in_background(future<> f) {
    engine().run_in_background(std::move(f));
}

void log_timer_callback_exception(std::exception_ptr ex) noexcept {
    seastar_logger.error("Timer callback failed: {}", std::current_exception());
}

}

#ifdef SEASTAR_TASK_BACKTRACE

void task::make_backtrace() noexcept {
    memory::disable_backtrace_temporarily dbt;
    try {
        _bt = make_lw_shared<simple_backtrace>(current_backtrace_tasklocal());
    } catch (...) {
        _bt = nullptr;
    }
}

#endif

}
