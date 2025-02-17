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
 * Copyright 2019 ScyllaDB
 */
#ifdef SEASTAR_MODULE
module;
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <utility>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <poll.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <boost/container/small_vector.hpp>
#include <fmt/core.h>
#include <seastar/util/assert.hh>

#ifdef SEASTAR_HAVE_URING
#include <liburing.h>
#endif

#ifdef SEASTAR_MODULE
module seastar;
#else
#include "core/reactor_backend.hh"
#include "core/thread_pool.hh"
#include "core/syscall_result.hh"
#include <seastar/core/internal/buffer_allocator.hh>
#include <seastar/util/internal/iovec_utils.hh>
#include <seastar/core/internal/uname.hh>
#include <seastar/core/print.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/read_first_line.hh>
#endif

namespace seastar {

using namespace std::chrono_literals;
using namespace internal;
using namespace internal::linux_abi;
namespace fs = std::filesystem;

class pollable_fd_state_completion : public kernel_completion {
    promise<> _pr;
public:
    virtual void complete_with(ssize_t res) override {
        _pr.set_value();
    }
    future<> get_future() {
        return _pr.get_future();
    }
};

void prepare_iocb(const io_request& req, io_completion* desc, iocb& iocb) {
    switch (req.opcode()) {
    case io_request::operation::fdatasync:
        iocb = make_fdsync_iocb(req.as<io_request::operation::fdatasync>().fd);
        break;
    case io_request::operation::write: {
        const auto& op = req.as<io_request::operation::write>();
        iocb = make_write_iocb(op.fd, op.pos, op.addr, op.size);
        set_nowait(iocb, op.nowait_works);
        break;
    }
    case io_request::operation::writev: {
        const auto& op = req.as<io_request::operation::writev>();
        iocb = make_writev_iocb(op.fd, op.pos, op.iovec, op.iov_len);
        set_nowait(iocb, op.nowait_works);
        break;
    }
    case io_request::operation::read: {
        const auto& op = req.as<io_request::operation::read>();
        iocb = make_read_iocb(op.fd, op.pos, op.addr, op.size);
        set_nowait(iocb, op.nowait_works);
        break;
    }
    case io_request::operation::readv: {
        const auto& op = req.as<io_request::operation::readv>();
        iocb = make_readv_iocb(op.fd, op.pos, op.iovec, op.iov_len);
        set_nowait(iocb, op.nowait_works);
        break;
    }
    default:
        seastar_logger.error("Invalid operation for iocb: {}", req.opname());
        std::abort();
    }
    set_user_data(iocb, desc);
}

aio_storage_context::iocb_pool::iocb_pool() {
    for (unsigned i = 0; i != max_aio; ++i) {
        _free_iocbs.push(&_iocb_pool[i]);
    }
}

aio_storage_context::aio_storage_context(reactor& r)
    : _r(r)
    , _io_context(0) {
    static_assert(max_aio >= reactor::max_queues * reactor::max_queues,
                  "Mismatch between maximum allowed io and what the IO queues can produce");
    internal::setup_aio_context(max_aio, &_io_context);
    _r.at_exit([this] { return stop(); });
}

aio_storage_context::~aio_storage_context() {
    internal::io_destroy(_io_context);
}

future<> aio_storage_context::stop() noexcept {
    return std::exchange(_pending_aio_retry_fut, make_ready_future<>()).finally([this] {
        return do_until([this] { return !_iocb_pool.outstanding(); }, [this] {
            reap_completions(false);
            return make_ready_future<>();
        });
    });
}

inline
internal::linux_abi::iocb&
aio_storage_context::iocb_pool::get_one() {
    auto io = _free_iocbs.top();
    _free_iocbs.pop();
    return *io;
}

inline
void
aio_storage_context::iocb_pool::put_one(internal::linux_abi::iocb* io) {
    _free_iocbs.push(io);
}

inline
unsigned
aio_storage_context::iocb_pool::outstanding() const {
    return max_aio - _free_iocbs.size();
}

inline
bool
aio_storage_context::iocb_pool::has_capacity() const {
    return !_free_iocbs.empty();
}

// Returns: number of iocbs consumed (0 or 1)
size_t
aio_storage_context::handle_aio_error(linux_abi::iocb* iocb, int ec) {
    switch (ec) {
        case EAGAIN:
            return 0;
        case EBADF: {
            auto desc = get_user_data<kernel_completion>(*iocb);
            _iocb_pool.put_one(iocb);
            desc->complete_with(-EBADF);
            // if EBADF, it means that the first request has a bad fd, so
            // we will only remove it from _pending_io and try again.
            return 1;
        }
        case EINVAL:
            // happens when the filesystem does not implement aio read or write
            [[fallthrough]];
        case ENOTSUP: {
            seastar_logger.error("io_submit failed: this happens when "
                                 "accessing filesystem which does not supports "
                                 "asynchronous direct I/O");
            auto desc = get_user_data<kernel_completion>(*iocb);
            _iocb_pool.put_one(iocb);
            desc->complete_with(-ENOTSUP);
            return 1;
        }
        default:
            ++_r._io_stats.aio_errors;
            throw std::system_error(ec, std::system_category(), "io_submit");
    }
}

extern bool aio_nowait_supported;

bool
aio_storage_context::submit_work() {
    bool did_work = false;

    _submission_queue.clear();
    size_t to_submit = _r._io_sink.drain([this] (const internal::io_request& req, io_completion* desc) -> bool {
        if (!_iocb_pool.has_capacity()) {
            return false;
        }

        auto& io = _iocb_pool.get_one();
        prepare_iocb(req, desc, io);

        if (_r._aio_eventfd) {
            set_eventfd_notification(io, _r._aio_eventfd->get_fd());
        }
        _submission_queue.push_back(&io);
        return true;
    });

    if (__builtin_expect(_r._cfg.kernel_page_cache, false)) {
        // linux-aio is not asynchronous when the page cache is used,
        // so we don't want to call io_submit() from the reactor thread.
        //
        // Pretend that all aio failed with EAGAIN and submit them
        // via schedule_retry(), below.
        did_work = !_submission_queue.empty();
        for (auto& iocbp : _submission_queue) {
            set_nowait(*iocbp, false);
            _pending_aio_retry.push_back(iocbp);
        }
        to_submit = 0;
    }

    size_t nr_consumed = 0;
    for (auto iocbs = _submission_queue.data(), end = iocbs + to_submit; iocbs < end; iocbs += nr_consumed) {
        auto nr = end - iocbs;
        auto r = io_submit(_io_context, nr, iocbs);
        if (r == -1) {
            nr_consumed = handle_aio_error(iocbs[0], errno);
        } else {
            nr_consumed = size_t(r);
        }
        did_work = true;
    }

    if (need_to_retry() && !retry_in_progress()) {
        schedule_retry();
    }

    return did_work;
}

void aio_storage_context::schedule_retry() {
    // loop until both _pending_aio_retry and _aio_retries are empty.
    // While retrying _aio_retries, new retries may be queued onto _pending_aio_retry.
    _pending_aio_retry_fut = do_until([this] {
        if (_aio_retries.empty()) {
            if (_pending_aio_retry.empty()) {
                return true;
            }
            // _pending_aio_retry, holding a batch of new iocbs to retry,
            // is swapped with the empty _aio_retries.
            std::swap(_aio_retries, _pending_aio_retry);
        }
        return false;
    }, [this] {
        return _r._thread_pool->submit<syscall_result<int>>([this] () mutable {
            auto r = io_submit(_io_context, _aio_retries.size(), _aio_retries.data());
            return wrap_syscall<int>(r);
        }).then_wrapped([this] (future<syscall_result<int>> f) {
            // If submit failed, just log the error and exit the loop.
            // The next call to submit_work will call schedule_retry again.
            if (f.failed()) {
                auto ex = f.get_exception();
                seastar_logger.warn("aio_storage_context::schedule_retry failed: {}", std::move(ex));
                return;
            }
            auto result = f.get();
            auto iocbs = _aio_retries.data();
            size_t nr_consumed = 0;
            if (result.result == -1) {
                try {
                    nr_consumed = handle_aio_error(iocbs[0], result.error);
                } catch (...) {
                    seastar_logger.error("aio retry failed: {}. Aborting.", std::current_exception());
                    abort();
                }
            } else {
                nr_consumed = result.result;
            }
            _aio_retries.erase(_aio_retries.begin(), _aio_retries.begin() + nr_consumed);
        });
    });
}

bool aio_storage_context::reap_completions(bool allow_retry)
{
    struct timespec timeout = {0, 0};
    auto n = io_getevents(_io_context, 1, max_aio, _ev_buffer, &timeout, _r._cfg.force_io_getevents_syscall);
    if (n == -1 && errno == EINTR) {
        n = 0;
    }
    SEASTAR_ASSERT(n >= 0);
    for (size_t i = 0; i < size_t(n); ++i) {
        auto iocb = get_iocb(_ev_buffer[i]);
        if (_ev_buffer[i].res == -EAGAIN && allow_retry) {
            set_nowait(*iocb, false);
            _pending_aio_retry.push_back(iocb);
            continue;
        }
        _iocb_pool.put_one(iocb);
        auto desc = get_user_data<kernel_completion>(_ev_buffer[i]);
        desc->complete_with(_ev_buffer[i].res);
    }
    return n;
}

bool aio_storage_context::can_sleep() const {
    // Because aio depends on polling, it cannot generate events to wake us up, Therefore, sleep
    // is only possible if there are no in-flight aios. If there are, we need to keep polling.
    //
    // Alternatively, if we enabled _aio_eventfd, we can always enter
    unsigned executing = _iocb_pool.outstanding();
    return executing == 0 || _r._aio_eventfd;
}

aio_general_context::aio_general_context(size_t nr)
        : iocbs(new iocb*[nr])
        , last(iocbs.get())
        , end(iocbs.get() + nr)
{
    setup_aio_context(nr, &io_context);
}

aio_general_context::~aio_general_context() {
    io_destroy(io_context);
}

void aio_general_context::queue(linux_abi::iocb* iocb) {
    SEASTAR_ASSERT(last < end);
    *last++ = iocb;
}

size_t aio_general_context::flush() {
    auto begin = iocbs.get();
    using clock = std::chrono::steady_clock;
    constexpr clock::time_point no_time_point = clock::time_point(clock::duration(0));
    auto retry_until = no_time_point;
    while (begin != last) {
        auto r = io_submit(io_context, last - begin, begin);
        if (__builtin_expect(r > 0, true)) {
            begin += r;
            continue;
        }
        // errno == EAGAIN is expected here. We don't explicitly assert that
        // since the assert below prevents an endless loop for any reason.
        if (retry_until == no_time_point) {
            // allow retrying for 1 second
            retry_until = clock::now() + 1s;
        } else {
            SEASTAR_ASSERT(clock::now() < retry_until);
        }
    }
    auto nr = last - iocbs.get();
    last = iocbs.get();
    return nr;
}

completion_with_iocb::completion_with_iocb(int fd, int events, void* user_data)
    : _iocb(make_poll_iocb(fd, events)) {
    set_user_data(_iocb, user_data);
}

void completion_with_iocb::maybe_queue(aio_general_context& context) {
    if (!_in_context) {
        _in_context = true;
        context.queue(&_iocb);
    }
}

hrtimer_aio_completion::hrtimer_aio_completion(reactor& r, file_desc& fd)
    : fd_kernel_completion(fd)
    , completion_with_iocb(fd.get(), POLLIN, this)
    , _r(r) {}

task_quota_aio_completion::task_quota_aio_completion(file_desc& fd)
    : fd_kernel_completion(fd)
    , completion_with_iocb(fd.get(), POLLIN, this) {}

smp_wakeup_aio_completion::smp_wakeup_aio_completion(file_desc& fd)
        : fd_kernel_completion(fd)
        , completion_with_iocb(fd.get(), POLLIN, this) {}

void
hrtimer_aio_completion::complete_with(ssize_t ret) {
    uint64_t expirations = 0;
    (void)_fd.read(&expirations, 8);
    if (expirations) {
        _r.service_highres_timer();
    }
    completion_with_iocb::completed();
}

void
task_quota_aio_completion::complete_with(ssize_t ret) {
    uint64_t v;
    (void)_fd.read(&v, 8);
    completion_with_iocb::completed();
}

void
smp_wakeup_aio_completion::complete_with(ssize_t ret) {
    uint64_t ignore = 0;
    (void)_fd.read(&ignore, 8);
    completion_with_iocb::completed();
}

preempt_io_context::preempt_io_context(reactor& r, file_desc& task_quota, file_desc& hrtimer)
    : _r(r)
    , _task_quota_aio_completion(task_quota)
    , _hrtimer_aio_completion(r, hrtimer)
{}

void preempt_io_context::start_tick() {
    // Preempt whenever an event (timer tick or signal) is available on the
    // _preempting_io ring
    set_need_preempt_var(reinterpret_cast<const preemption_monitor*>(_context.io_context + 8));
    // preempt_io_context::request_preemption() will write to reactor::_preemption_monitor, which is now ignored
}

void preempt_io_context::stop_tick() {
    set_need_preempt_var(&_r._preemption_monitor);
}

void preempt_io_context::request_preemption() {
    ::itimerspec expired = {};
    expired.it_value.tv_nsec = 1;
    // will trigger immediately, triggering the preemption monitor
    _hrtimer_aio_completion.fd().timerfd_settime(TFD_TIMER_ABSTIME, expired);

    // This might have been called from poll_once. If that is the case, we cannot assume that timerfd is being
    // monitored.
    _hrtimer_aio_completion.maybe_queue(_context);
    _context.flush();

    // The kernel is not obliged to deliver the completion immediately, so wait for it
    while (!need_preempt()) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
}

void preempt_io_context::reset_preemption_monitor() {
    service_preempting_io();
    _hrtimer_aio_completion.maybe_queue(_context);
    _task_quota_aio_completion.maybe_queue(_context);
    flush();
}

bool preempt_io_context::service_preempting_io() {
    linux_abi::io_event a[2];
    auto r = io_getevents(_context.io_context, 0, 2, a, 0);
    SEASTAR_ASSERT(r != -1);
    bool did_work = r > 0;
    for (unsigned i = 0; i != unsigned(r); ++i) {
        auto desc = get_user_data<kernel_completion>(a[i]);
        desc->complete_with(a[i].res);
    }
    return did_work;
}

file_desc reactor_backend_aio::make_timerfd() {
    return file_desc::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC|TFD_NONBLOCK);
}

bool reactor_backend_aio::await_events(int timeout, const sigset_t* active_sigmask) {
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
        const bool may_sleep = !tsp || (tsp->tv_nsec + tsp->tv_sec > 0);
        const auto before_getevents = may_sleep ? sched_clock::now() : sched_clock::time_point{};
        r = io_pgetevents(_polling_io.io_context, 1, batch_size, batch, tsp, active_sigmask);
        if (may_sleep) {
            _r._total_sleep += sched_clock::now() - before_getevents;
        }
        if (r == -1 && errno == EINTR) {
            return true;
        }
        SEASTAR_ASSERT(r != -1);
        for (unsigned i = 0; i != unsigned(r); ++i) {
            did_work = true;
            auto& event = batch[i];
            auto* desc = get_user_data<kernel_completion>(event);
            desc->complete_with(event.res);
        }
        // For the next iteration, don't use a timeout, since we may have waited already
        ts = {};
        tsp = &ts;
    } while (r == batch_size);
    return did_work;
}

void reactor_backend_aio::signal_received(int signo, siginfo_t* siginfo, void* ignore) {
    _r._signals.action(signo, siginfo, ignore);
}

reactor_backend_aio::reactor_backend_aio(reactor& r)
    : _r(r)
    , _hrtimer_timerfd(make_timerfd())
    , _storage_context(_r)
    , _preempting_io(_r, _r._task_quota_timer, _hrtimer_timerfd)
    , _polling_io(_r._cfg.max_networking_aio_io_control_blocks)
    , _hrtimer_poll_completion(_r, _hrtimer_timerfd)
    , _smp_wakeup_aio_completion(_r._notify_eventfd)
{
    // Protect against spurious wakeups - if we get notified that the timer has
    // expired when it really hasn't, we don't want to block in read(tfd, ...).
    auto tfd = _r._task_quota_timer.get();
    ::fcntl(tfd, F_SETFL, ::fcntl(tfd, F_GETFL) | O_NONBLOCK);

    sigset_t mask = make_sigset_mask(hrtimer_signal());
    auto e = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
    SEASTAR_ASSERT(e == 0);
}

bool reactor_backend_aio::reap_kernel_completions() {
    bool did_work = await_events(0, nullptr);
    did_work |= _storage_context.reap_completions();
    return did_work;
}

bool reactor_backend_aio::kernel_submit_work() {
    _hrtimer_poll_completion.maybe_queue(_polling_io);
    bool did_work = _polling_io.flush();
    did_work |= _storage_context.submit_work();
    return did_work;
}

bool reactor_backend_aio::kernel_events_can_sleep() const {
    return _storage_context.can_sleep();
}

void reactor_backend_aio::wait_and_process_events(const sigset_t* active_sigmask) {
    int timeout = -1;
    bool did_work = _preempting_io.service_preempting_io();
    if (did_work) {
        timeout = 0;
    }

    _hrtimer_poll_completion.maybe_queue(_polling_io);
    _smp_wakeup_aio_completion.maybe_queue(_polling_io);
    _polling_io.flush();
    await_events(timeout, active_sigmask);
    _preempting_io.service_preempting_io(); // clear task quota timer
}

class aio_pollable_fd_state : public pollable_fd_state {
    internal::linux_abi::iocb _iocb_pollin;
    pollable_fd_state_completion _completion_pollin;

    internal::linux_abi::iocb _iocb_pollout;
    pollable_fd_state_completion _completion_pollout;

    internal::linux_abi::iocb _iocb_pollrdhup;
    pollable_fd_state_completion _completion_pollrdhup;
public:
    pollable_fd_state_completion* get_desc(int events) {
        if (events & POLLIN) {
            return &_completion_pollin;
        }
        if (events & POLLOUT) {
            return &_completion_pollout;
        }
        return &_completion_pollrdhup;
    }
    internal::linux_abi::iocb* get_iocb(int events) {
        if (events & POLLIN) {
            return &_iocb_pollin;
        }
        if (events & POLLOUT) {
            return &_iocb_pollout;
        }
        return &_iocb_pollrdhup;
    }
    explicit aio_pollable_fd_state(file_desc fd, speculation speculate)
        : pollable_fd_state(std::move(fd), std::move(speculate))
    {}
    future<> get_completion_future(int events) {
        return get_desc(events)->get_future();
    }
};

future<> reactor_backend_aio::poll(pollable_fd_state& fd, int events) {
    try {
        if (events & fd.events_known) {
            fd.events_known &= ~events;
            return make_ready_future<>();
        }

        fd.events_rw = events == (POLLIN|POLLOUT);

        auto* pfd = static_cast<aio_pollable_fd_state*>(&fd);
        auto* iocb = pfd->get_iocb(events);
        auto* desc = pfd->get_desc(events);
        *iocb = make_poll_iocb(fd.fd.get(), events);
        *desc = pollable_fd_state_completion{};
        set_user_data(*iocb, desc);
        _polling_io.queue(iocb);
        return pfd->get_completion_future(events);
    } catch (...) {
        return make_exception_future<>(std::current_exception());
    }
}

future<> reactor_backend_aio::readable(pollable_fd_state& fd) {
    return poll(fd, POLLIN);
}

future<> reactor_backend_aio::writeable(pollable_fd_state& fd) {
    return poll(fd, POLLOUT);
}

future<> reactor_backend_aio::readable_or_writeable(pollable_fd_state& fd) {
    return poll(fd, POLLIN|POLLOUT);
}

future<> reactor_backend_aio::poll_rdhup(pollable_fd_state& fd) {
    return poll(fd, POLLRDHUP);
}

void reactor_backend_aio::forget(pollable_fd_state& fd) noexcept {
    auto* pfd = static_cast<aio_pollable_fd_state*>(&fd);
    delete pfd;
    // ?
}

future<std::tuple<pollable_fd, socket_address>>
reactor_backend_aio::accept(pollable_fd_state& listenfd) {
    return _r.do_accept(listenfd);
}

future<> reactor_backend_aio::connect(pollable_fd_state& fd, socket_address& sa) {
    return _r.do_connect(fd, sa);
}

void reactor_backend_aio::shutdown(pollable_fd_state& fd, int how) {
    fd.fd.shutdown(how);
}

future<size_t>
reactor_backend_aio::read(pollable_fd_state& fd, void* buffer, size_t len) {
    return _r.do_read(fd, buffer, len);
}

future<size_t>
reactor_backend_aio::recvmsg(pollable_fd_state& fd, const std::vector<iovec>& iov) {
    return _r.do_recvmsg(fd, iov);
}

future<temporary_buffer<char>>
reactor_backend_aio::read_some(pollable_fd_state& fd, internal::buffer_allocator* ba) {
    return _r.do_read_some(fd, ba);
}

future<size_t>
reactor_backend_aio::send(pollable_fd_state& fd, const void* buffer, size_t len) {
    return _r.do_send(fd, buffer, len);
}

future<size_t>
reactor_backend_aio::sendmsg(pollable_fd_state& fd, net::packet& p) {
    return _r.do_sendmsg(fd, p);
}

future<temporary_buffer<char>>
reactor_backend_aio::recv_some(pollable_fd_state& fd, internal::buffer_allocator* ba) {
    return _r.do_recv_some(fd, ba);
}

void reactor_backend_aio::start_tick() {
    _preempting_io.start_tick();
}

void reactor_backend_aio::stop_tick() {
    _preempting_io.stop_tick();
}

void reactor_backend_aio::arm_highres_timer(const ::itimerspec& its) {
    _hrtimer_timerfd.timerfd_settime(TFD_TIMER_ABSTIME, its);
}

void reactor_backend_aio::reset_preemption_monitor() {
    _preempting_io.reset_preemption_monitor();
}

void reactor_backend_aio::request_preemption() {
    _preempting_io.request_preemption();
}

void reactor_backend_aio::start_handling_signal() {
    // The aio backend only uses SIGHUP/SIGTERM/SIGINT. We don't need to handle them right away and our
    // implementation of request_preemption is not signal safe, so do nothing.
}

pollable_fd_state_ptr
reactor_backend_aio::make_pollable_fd_state(file_desc fd, pollable_fd::speculation speculate) {
    return pollable_fd_state_ptr(new aio_pollable_fd_state(std::move(fd), std::move(speculate)));
}

reactor_backend_epoll::reactor_backend_epoll(reactor& r)
        : _r(r)
        , _steady_clock_timer_reactor_thread(file_desc::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC))
        , _steady_clock_timer_timer_thread(file_desc::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC))
        , _epollfd(file_desc::epoll_create(EPOLL_CLOEXEC))
        , _storage_context(_r) {
    ::epoll_event event;
    event.events = EPOLLIN;
    event.data.ptr = nullptr;
    auto ret = ::epoll_ctl(_epollfd.get(), EPOLL_CTL_ADD, _r._notify_eventfd.get(), &event);
    throw_system_error_on(ret == -1);
    event.events = EPOLLIN;
    event.data.ptr = &_steady_clock_timer_reactor_thread;
    ret = ::epoll_ctl(_epollfd.get(), EPOLL_CTL_ADD, _steady_clock_timer_reactor_thread.get(), &event);
    throw_system_error_on(ret == -1);
}

void
reactor_backend_epoll::task_quota_timer_thread_fn() {
    auto thread_name = seastar::format("timer-{}", _r._id);
    pthread_setname_np(pthread_self(), thread_name.c_str());

    sigset_t mask;
    sigfillset(&mask);
    for (auto sig : { SIGSEGV }) {
        sigdelset(&mask, sig);
    }
    auto r = ::pthread_sigmask(SIG_BLOCK, &mask, NULL);
    if (r) {
        seastar_logger.error("Thread {}: failed to block signals. Aborting.", thread_name.c_str());
        abort();
    }

    // We need to wait until task quota is set before we can calculate how many ticks are to
    // a minute. Technically task_quota is used from many threads, but since it is read-only here
    // and only used during initialization we will avoid complicating the code.
    {
        uint64_t events;
        _r._task_quota_timer.read(&events, 8);
        _r.request_preemption();
    }

    while (!_r._dying.load(std::memory_order_relaxed)) {
        // Wait for either the task quota timer, or the high resolution timer, or both,
        // to expire.
        struct pollfd pfds[2] = {};
        pfds[0].fd = _r._task_quota_timer.get();
        pfds[0].events = POLL_IN;
        pfds[1].fd = _steady_clock_timer_timer_thread.get();
        pfds[1].events = POLL_IN;
        int r = poll(pfds, 2, -1);
        SEASTAR_ASSERT(r != -1);

        uint64_t events;
        if (pfds[0].revents & POLL_IN) {
            _r._task_quota_timer.read(&events, 8);
        }
        if (pfds[1].revents & POLL_IN) {
            _steady_clock_timer_timer_thread.read(&events, 8);
            _highres_timer_pending.store(true, std::memory_order_relaxed);
        }
        _r.request_preemption();

        // We're in a different thread, but guaranteed to be on the same core, so even
        // a signal fence is overdoing it
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
}

reactor_backend_epoll::~reactor_backend_epoll() = default;

void reactor_backend_epoll::start_tick() {
    _task_quota_timer_thread = std::thread(&reactor_backend_epoll::task_quota_timer_thread_fn, this);

    ::sched_param sp;
    sp.sched_priority = 1;
    auto sched_ok = pthread_setschedparam(_task_quota_timer_thread.native_handle(), SCHED_FIFO, &sp);
    if (sched_ok != 0 && _r._id == 0) {
        seastar_logger.warn("Unable to set SCHED_FIFO scheduling policy for timer thread; latency impact possible. Try adding CAP_SYS_NICE");
    }
}

void reactor_backend_epoll::stop_tick() {
    _r._dying.store(true, std::memory_order_relaxed);
    _r._task_quota_timer.timerfd_settime(0, seastar::posix::to_relative_itimerspec(1ns, 1ms)); // Make the timer fire soon
    _task_quota_timer_thread.join();
}

void reactor_backend_epoll::arm_highres_timer(const ::itimerspec& its) {
    _steady_clock_timer_deadline = its;
    _steady_clock_timer_timer_thread.timerfd_settime(TFD_TIMER_ABSTIME, its);
}

void
reactor_backend_epoll::switch_steady_clock_timers(file_desc& from, file_desc& to) {
    auto& deadline = _steady_clock_timer_deadline;
    if (deadline.it_value.tv_sec == 0 && deadline.it_value.tv_nsec == 0) {
        return;
    }
    // Enable-then-disable, so the hardware timer doesn't have to be reprogrammed. Probably pointless.
    to.timerfd_settime(TFD_TIMER_ABSTIME, _steady_clock_timer_deadline);
    from.timerfd_settime(TFD_TIMER_ABSTIME, {});
}

void reactor_backend_epoll::maybe_switch_steady_clock_timers(int timeout, file_desc& from, file_desc& to) {
    if (timeout != 0) {
        switch_steady_clock_timers(from, to);
    }
}

bool
reactor_backend_epoll::wait_and_process(int timeout, const sigset_t* active_sigmask) {
    // If we plan to sleep, disable the timer thread steady clock timer (since it won't
    // wake us up from sleep, and timer thread wakeup will just waste CPU time) and enable
    // reactor thread steady clock timer.
    maybe_switch_steady_clock_timers(timeout, _steady_clock_timer_timer_thread, _steady_clock_timer_reactor_thread);
    auto undo_timer_switch = defer([&] () noexcept {
      try {
        maybe_switch_steady_clock_timers(timeout, _steady_clock_timer_reactor_thread, _steady_clock_timer_timer_thread);
      } catch (...) {
        seastar_logger.error("Switching steady_clock timers back failed: {}. Aborting...", std::current_exception());
        abort();
      }
    });
    std::array<epoll_event, 128> eevt;
    const auto before_pwait = sched_clock::now();
    int nr = ::epoll_pwait(_epollfd.get(), eevt.data(), eevt.size(), timeout, active_sigmask);
    _r._total_sleep += sched_clock::now() - before_pwait;
    if (nr == -1 && errno == EINTR) {
        return false; // gdb can cause this
    }
    SEASTAR_ASSERT(nr != -1);
    for (int i = 0; i < nr; ++i) {
        auto& evt = eevt[i];
        auto pfd = reinterpret_cast<pollable_fd_state*>(evt.data.ptr);
        if (!pfd) {
            char dummy[8];
            _r._notify_eventfd.read(dummy, 8);
            continue;
        }
        if (evt.data.ptr == &_steady_clock_timer_reactor_thread) {
            char dummy[8];
            _steady_clock_timer_reactor_thread.read(dummy, 8);
            _highres_timer_pending.store(true, std::memory_order_relaxed);
            _steady_clock_timer_deadline = {};
            continue;
        }
        bool has_error = evt.events & (EPOLLHUP | EPOLLERR);
        if (has_error) {
            // treat the events as required events when error occurs, let
            // send/recv/accept/connect handle the specific error.
            evt.events = pfd->events_requested;
        }
        auto events = evt.events & (EPOLLIN | EPOLLOUT | EPOLLRDHUP);
        auto events_to_remove = has_error ? pfd->events_requested : events & ~pfd->events_requested;
        complete_epoll_event(*pfd, events, EPOLLRDHUP);
        if (pfd->events_rw) {
            // accept() signals normal completions via EPOLLIN, but errors (due to shutdown())
            // via EPOLLOUT|EPOLLHUP, so we have to wait for both EPOLLIN and EPOLLOUT with the
            // same future
            complete_epoll_event(*pfd, events, EPOLLIN|EPOLLOUT);
        } else {
            // Normal processing where EPOLLIN and EPOLLOUT are waited for via different
            // futures.
            complete_epoll_event(*pfd, events, EPOLLIN);
            complete_epoll_event(*pfd, events, EPOLLOUT);
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

class epoll_pollable_fd_state : public pollable_fd_state {
    pollable_fd_state_completion _pollin;
    pollable_fd_state_completion _pollout;
    pollable_fd_state_completion _pollrdhup;

    pollable_fd_state_completion* get_desc(int events) {
        if (events & EPOLLIN) {
            return &_pollin;
        }
        if (events & EPOLLOUT) {
            return &_pollout;
        }
        return &_pollrdhup;
    }
public:
    explicit epoll_pollable_fd_state(file_desc fd, speculation speculate)
        : pollable_fd_state(std::move(fd), std::move(speculate))
    {}
    future<> get_completion_future(int event) {
        auto desc = get_desc(event);
        *desc = pollable_fd_state_completion{};
        return desc->get_future();
    }

    void complete_with(int event) {
        get_desc(event)->complete_with(event);
    }
};

bool reactor_backend_epoll::reap_kernel_completions() {
    // epoll does not have a separate submission stage, and just
    // calls epoll_ctl everytime it needs, so this method and
    // kernel_submit_work are essentially the same. Ordering also
    // doesn't matter much. wait_and_process is actually completing,
    // but we prefer to call it in kernel_submit_work because the
    // reactor register two pollers for completions and one for submission,
    // since completion is cheaper for other backends like aio. This avoids
    // calling epoll_wait twice.
    //
    // We will only reap the io completions
    return _storage_context.reap_completions();
}

bool reactor_backend_epoll::kernel_submit_work() {
    bool result = false;
    _storage_context.submit_work();
    if (_need_epoll_events) {
        result |= wait_and_process(0, nullptr);
    }

    result |= complete_hrtimer();

    return result;
}

bool reactor_backend_epoll::complete_hrtimer() {
    // This can be set from either the task quota timer thread, or
    // wait_and_process(), above.
    if (_highres_timer_pending.load(std::memory_order_relaxed)) {
        _highres_timer_pending.store(false, std::memory_order_relaxed);
        _r.service_highres_timer();
        return true;
    }
    return false;
}

bool reactor_backend_epoll::kernel_events_can_sleep() const {
    return _storage_context.can_sleep();
}

void reactor_backend_epoll::wait_and_process_events(const sigset_t* active_sigmask) {
    wait_and_process(-1 , active_sigmask);
    complete_hrtimer();
}

void reactor_backend_epoll::complete_epoll_event(pollable_fd_state& pfd, int events, int event) {
    if (pfd.events_requested & events & event) {
        pfd.events_requested &= ~event;
        pfd.events_known &= ~event;
        auto* fd = static_cast<epoll_pollable_fd_state*>(&pfd);
        return fd->complete_with(event);
    }
}

void reactor_backend_epoll::signal_received(int signo, siginfo_t* siginfo, void* ignore) {
    if (engine_is_ready()) {
        _r._signals.action(signo, siginfo, ignore);
    } else {
        reactor::signals::failed_to_handle(signo);
    }
}

future<> reactor_backend_epoll::get_epoll_future(pollable_fd_state& pfd, int event) {
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
        SEASTAR_ASSERT(r == 0);
        _need_epoll_events = true;
    }

    auto* fd = static_cast<epoll_pollable_fd_state*>(&pfd);
    return fd->get_completion_future(event);
}

future<> reactor_backend_epoll::readable(pollable_fd_state& fd) {
    return get_epoll_future(fd, EPOLLIN);
}

future<> reactor_backend_epoll::writeable(pollable_fd_state& fd) {
    return get_epoll_future(fd, EPOLLOUT);
}

future<> reactor_backend_epoll::readable_or_writeable(pollable_fd_state& fd) {
    return get_epoll_future(fd, EPOLLIN | EPOLLOUT);
}

future<> reactor_backend_epoll::poll_rdhup(pollable_fd_state& fd) {
    return get_epoll_future(fd, POLLRDHUP);
}

void reactor_backend_epoll::forget(pollable_fd_state& fd) noexcept {
    if (fd.events_epoll) {
        ::epoll_ctl(_epollfd.get(), EPOLL_CTL_DEL, fd.fd.get(), nullptr);
    }
    auto* efd = static_cast<epoll_pollable_fd_state*>(&fd);
    delete efd;
}

future<std::tuple<pollable_fd, socket_address>>
reactor_backend_epoll::accept(pollable_fd_state& listenfd) {
    return _r.do_accept(listenfd);
}

future<> reactor_backend_epoll::connect(pollable_fd_state& fd, socket_address& sa) {
    return _r.do_connect(fd, sa);
}

void reactor_backend_epoll::shutdown(pollable_fd_state& fd, int how) {
    fd.fd.shutdown(how);
}

future<size_t>
reactor_backend_epoll::read(pollable_fd_state& fd, void* buffer, size_t len) {
    return _r.do_read(fd, buffer, len);
}

future<size_t>
reactor_backend_epoll::recvmsg(pollable_fd_state& fd, const std::vector<iovec>& iov) {
    return _r.do_recvmsg(fd, iov);
}

future<temporary_buffer<char>>
reactor_backend_epoll::read_some(pollable_fd_state& fd, internal::buffer_allocator* ba) {
    return _r.do_read_some(fd, ba);
}

future<size_t>
reactor_backend_epoll::send(pollable_fd_state& fd, const void* buffer, size_t len) {
    return _r.do_send(fd, buffer, len);
}

future<size_t>
reactor_backend_epoll::sendmsg(pollable_fd_state& fd, net::packet& p) {
    return _r.do_sendmsg(fd, p);
}

future<temporary_buffer<char>>
reactor_backend_epoll::recv_some(pollable_fd_state& fd, internal::buffer_allocator* ba) {
    return _r.do_recv_some(fd, ba);
}

void
reactor_backend_epoll::request_preemption() {
    _r._preemption_monitor.head.store(1, std::memory_order_relaxed);
}

void reactor_backend_epoll::start_handling_signal() {
    // The epoll backend uses signals for the high resolution timer. That is used for thread_scheduling_group, so we
    // request preemption so when we receive a signal.
    request_preemption();
}

pollable_fd_state_ptr
reactor_backend_epoll::make_pollable_fd_state(file_desc fd, pollable_fd::speculation speculate) {
    return pollable_fd_state_ptr(new epoll_pollable_fd_state(std::move(fd), std::move(speculate)));
}

void reactor_backend_epoll::reset_preemption_monitor() {
    _r._preemption_monitor.head.store(0, std::memory_order_relaxed);
}

#ifdef SEASTAR_HAVE_URING

static
std::optional<::io_uring>
try_create_uring(unsigned queue_len, bool throw_on_error) {
    auto required_features =
            IORING_FEAT_SUBMIT_STABLE
            | IORING_FEAT_NODROP;
    auto required_ops = {
            IORING_OP_POLL_ADD, // linux 5.1
            IORING_OP_READV,
            IORING_OP_WRITEV,
            IORING_OP_FSYNC,
            IORING_OP_SENDMSG,  // linux 5.3
            IORING_OP_RECVMSG,
            IORING_OP_ACCEPT,
            IORING_OP_CONNECT,
            IORING_OP_READ,     // linux 5.6
            IORING_OP_WRITE,
            IORING_OP_SEND,
            IORING_OP_RECV,
            };
    auto maybe_throw = [&] (auto exception) {
        if (throw_on_error) {
            throw exception;
        }
    };

    auto params = ::io_uring_params{};
    ::io_uring ring;
    auto err = ::io_uring_queue_init_params(queue_len, &ring, &params);
    if (err != 0) {
        maybe_throw(std::system_error(std::error_code(-err, std::system_category()), "trying to create io_uring"));
        return std::nullopt;
    }
    auto free_ring = defer([&] () noexcept { ::io_uring_queue_exit(&ring); });
    ::io_uring_ring_dontfork(&ring);
    if (~ring.features & required_features) {
        maybe_throw(std::runtime_error(fmt::format("missing required io_ring features, required 0x{:x} available 0x{:x}", required_features, ring.features)));
        return std::nullopt;
    }

    auto probe = ::io_uring_get_probe_ring(&ring);
    if (!probe) {
        maybe_throw(std::runtime_error("unable to create io_uring probe"));
        return std::nullopt;
    }
    auto free_probe = defer([&] () noexcept { ::io_uring_free_probe(probe); });

    for (auto op : required_ops) {
        if (!io_uring_opcode_supported(probe, op)) {
            maybe_throw(std::runtime_error(fmt::format("required io_uring opcode {} not supported", static_cast<int>(op))));
            return std::nullopt;
        }
    }
    free_ring.cancel();

    return ring;
}

static
bool
have_md_devices() {
    namespace fs = std::filesystem;
    for (auto entry : fs::directory_iterator("/sys/block")) {
        if (entry.is_directory() && fs::exists(entry.path() / "md")) {
            return true;
        }
    }
    return false;
}

static size_t mlock_limit() {
    struct ::rlimit lim;
    int r = ::getrlimit(RLIMIT_MEMLOCK, &lim);
    if (r == -1) {
        return 0; // assume the worst; this is advisory anyway
    }
    return lim.rlim_cur;
}

static
bool
detect_io_uring() {
    if (!kernel_uname().whitelisted({"5.17"}) && have_md_devices()) {
        // Older kernels fall back to workqueues for RAID devices
        return false;
    }
    if (!kernel_uname().whitelisted({"5.12"}) && mlock_limit() < (8 << 20)) {
        // Older kernels lock about 32k/vcpu for the ring itself. Require 8MB of
        // locked memory to be safe (8MB is what newer kernels and newer systemd provide)
        return false;
    }
    auto ring_opt = try_create_uring(1, false);
    if (ring_opt) {
        ::io_uring_queue_exit(&ring_opt.value());
    }
    return bool(ring_opt);
}

class reactor_backend_uring final : public reactor_backend {
    // s_queue_len is more or less arbitrary. Too low and we'll be
    // issuing too small batches, too high and we require too much locked
    // memory, but otherwise it doesn't matter.
    static constexpr unsigned s_queue_len = 200;
    reactor& _r;
    ::io_uring _uring;
    bool _did_work_while_getting_sqe = false;
    bool _has_pending_submissions = false;
    file_desc _hrtimer_timerfd;
    preempt_io_context _preempt_io_context;

    class uring_pollable_fd_state : public pollable_fd_state {
        pollable_fd_state_completion _completion_pollin;
        pollable_fd_state_completion _completion_pollout;
        pollable_fd_state_completion _completion_pollrdhup;
    public:
        explicit uring_pollable_fd_state(file_desc desc, speculation speculate)
                : pollable_fd_state(std::move(desc), std::move(speculate)) {
        }
        pollable_fd_state_completion* get_desc(int events) {
            if (events & POLLIN) {
                return &_completion_pollin;
            } else if (events & POLLOUT) {
                return &_completion_pollout;
            } else {
                return &_completion_pollrdhup;
            }
        }
        future<> get_completion_future(int events) {
            return get_desc(events)->get_future();
        }
    };

    // eventfd and timerfd both need an 8-byte read after completion
    class recurring_eventfd_or_timerfd_completion : public fd_kernel_completion {
        bool _armed = false;
    public:
        explicit recurring_eventfd_or_timerfd_completion(file_desc& fd) : fd_kernel_completion(fd) {}
        virtual void complete_with(ssize_t res) override {
            char garbage[8];
            auto ret = _fd.read(garbage, 8);
            // Note: for hrtimer_completion we can have spurious wakeups,
            // since we wait for this using both _preempt_io_context and the
            // ring. So don't assert that we read anything.
            SEASTAR_ASSERT(!ret || *ret == 8);
            _armed = false;
        }
        void maybe_rearm(reactor_backend_uring& be) {
            if (_armed) {
                return;
            }
            auto sqe = be.get_sqe();
            ::io_uring_prep_poll_add(sqe, fd().get(), POLLIN);
            ::io_uring_sqe_set_data(sqe, static_cast<kernel_completion*>(this));
            _armed = true;
            be._has_pending_submissions = true;
        }
    };

    // Completion for high resolution timerfd, used in wait_and_process_events()
    // (while running tasks it's waited for in _preempt_io_context)
    class hrtimer_completion : public recurring_eventfd_or_timerfd_completion {
        reactor& _r;
    public:
        explicit hrtimer_completion(reactor& r, file_desc& timerfd)
                : recurring_eventfd_or_timerfd_completion(timerfd), _r(r) {
        }
        virtual void complete_with(ssize_t res) override {
            recurring_eventfd_or_timerfd_completion::complete_with(res);
            _r.service_highres_timer();
        }
    };

    using smp_wakeup_completion = recurring_eventfd_or_timerfd_completion;

    hrtimer_completion _hrtimer_completion;
    smp_wakeup_completion _smp_wakeup_completion;
private:
    static file_desc make_timerfd() {
        return file_desc::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC|TFD_NONBLOCK);
    }

    // Can fail if the completion queue is full
    ::io_uring_sqe* try_get_sqe() {
        return ::io_uring_get_sqe(&_uring);
    }

    bool do_flush_submission_ring() {
        if (_has_pending_submissions) {
            _has_pending_submissions = false;
            _did_work_while_getting_sqe = false;
            io_uring_submit(&_uring);
            return true;
        } else {
            return std::exchange(_did_work_while_getting_sqe, false);
        }
    }

    ::io_uring_sqe* get_sqe() {
        ::io_uring_sqe* sqe;
        while (__builtin_expect((sqe = try_get_sqe()) == nullptr, false)) {
            do_flush_submission_ring();
            do_process_kernel_completions_step();
            _did_work_while_getting_sqe = true;
        }
        return sqe;
    }

    future<> poll(pollable_fd_state& fd, int events) {
        auto sqe = get_sqe();
        ::io_uring_prep_poll_add(sqe, fd.fd.get(), events);
        auto ufd = static_cast<uring_pollable_fd_state*>(&fd);
        ::io_uring_sqe_set_data(sqe, static_cast<kernel_completion*>(ufd->get_desc(events)));
        _has_pending_submissions = true;
        return ufd->get_completion_future(events);
    }

    void submit_io_request(const internal::io_request& req, io_completion* completion) {
        auto sqe = get_sqe();
        using o = internal::io_request::operation;
        switch (req.opcode()) {
            case o::read: {
                const auto& op = req.as<io_request::operation::read>();
                ::io_uring_prep_read(sqe, op.fd, op.addr, op.size, op.pos);
                break;
            }
            case o::write: {
                const auto& op = req.as<io_request::operation::write>();
                ::io_uring_prep_write(sqe, op.fd, op.addr, op.size, op.pos);
                break;
            }
            case o::readv: {
                const auto& op = req.as<io_request::operation::readv>();
                ::io_uring_prep_readv(sqe, op.fd, op.iovec, op.iov_len, op.pos);
                break;
            }
            case o::writev: {
                const auto& op = req.as<io_request::operation::writev>();
                ::io_uring_prep_writev(sqe, op.fd, op.iovec, op.iov_len, op.pos);
                break;
            }
            case o::fdatasync: {
                const auto& op = req.as<io_request::operation::fdatasync>();
                ::io_uring_prep_fsync(sqe, op.fd, IORING_FSYNC_DATASYNC);
                break;
            }
            case o::recv: {
                const auto& op = req.as<io_request::operation::recv>();
                ::io_uring_prep_recv(sqe, op.fd, op.addr, op.size, op.flags);
                break;
            }
            case o::recvmsg: {
                const auto& op = req.as<io_request::operation::recvmsg>();
                ::io_uring_prep_recvmsg(sqe, op.fd, op.msghdr, op.flags);
                break;
            }
            case o::send: {
                const auto& op = req.as<io_request::operation::send>();
                ::io_uring_prep_send(sqe, op.fd, op.addr, op.size, op.flags);
                break;
            }
            case o::sendmsg: {
                const auto& op = req.as<io_request::operation::sendmsg>();
                ::io_uring_prep_sendmsg(sqe, op.fd, op.msghdr, op.flags);
                break;
            }
            case o::accept: {
                const auto& op = req.as<io_request::operation::accept>();
                ::io_uring_prep_accept(sqe, op.fd, op.sockaddr, op.socklen_ptr, op.flags);
                break;
            }
            case o::connect: {
                const auto& op = req.as<io_request::operation::connect>();
                ::io_uring_prep_connect(sqe, op.fd, op.sockaddr, op.socklen);
                break;
            }
            case o::poll_add:
            case o::poll_remove:
            case o::cancel:
                // The reactor does not generate these types of I/O requests yet, so
                // this path is unreachable. As more features of io_uring are exploited,
                // we'll utilize more of these opcodes.
                seastar_logger.error("Invalid operation for iocb: {}", req.opname());
                abort();
        }
        ::io_uring_sqe_set_data(sqe, completion);

        _has_pending_submissions = true;
    }

    // Returns true if any work was done
    bool queue_pending_file_io() {
        return _r._io_sink.drain([&] (const internal::io_request& req, io_completion* completion) -> bool {
            submit_io_request(req, completion);
            return true;
        });
    }

    // Process kernel completions already extracted from the ring.
    // This is needed because we sometimes extract completions without
    // waiting, and sometimes with waiting.
    void do_process_ready_kernel_completions(::io_uring_cqe** buf, size_t nr) {
        for (auto p = buf; p != buf + nr; ++p) {
            auto cqe = *p;
            auto completion = reinterpret_cast<kernel_completion*>(cqe->user_data);
            completion->complete_with(cqe->res);
        }
    }

    // Returns true if completions were processed
    bool do_process_kernel_completions_step() {
        struct ::io_uring_cqe* buf[s_queue_len];
        auto n = ::io_uring_peek_batch_cqe(&_uring, buf, s_queue_len);
        do_process_ready_kernel_completions(buf, n);
        ::io_uring_cq_advance(&_uring, n);
        return n != 0;
    }

    // Returns true if completions were processed
    bool do_process_kernel_completions() {
        auto did_work = false;
        while (do_process_kernel_completions_step()) {
            did_work = true;
        }
        return did_work | std::exchange(_did_work_while_getting_sqe, false);
    }

    template<typename Completion>
    auto submit_request(std::unique_ptr<Completion> desc, io_request&& req) noexcept {
        auto fut = desc->get_future();
        _r._io_sink.submit(desc.release(), std::move(req));
        return fut;
    }
public:
    explicit reactor_backend_uring(reactor& r)
            : _r(r)
            , _uring(try_create_uring(s_queue_len, true).value())
            , _hrtimer_timerfd(make_timerfd())
            , _preempt_io_context(_r, _r._task_quota_timer, _hrtimer_timerfd)
            , _hrtimer_completion(_r, _hrtimer_timerfd)
            , _smp_wakeup_completion(_r._notify_eventfd) {
        // Protect against spurious wakeups - if we get notified that the timer has
        // expired when it really hasn't, we don't want to block in read(tfd, ...).
        auto tfd = _r._task_quota_timer.get();
        ::fcntl(tfd, F_SETFL, ::fcntl(tfd, F_GETFL) | O_NONBLOCK);
    }
    ~reactor_backend_uring() {
        ::io_uring_queue_exit(&_uring);
    }
    virtual bool reap_kernel_completions() override {
        return do_process_kernel_completions();
    }
    virtual bool kernel_submit_work() override {
        bool did_work = false;
        did_work |= _preempt_io_context.service_preempting_io();
        did_work |= queue_pending_file_io();
        did_work |= ::io_uring_submit(&_uring);
        return did_work;
    }
    virtual bool kernel_events_can_sleep() const override {
        // We never need to spin while I/O is in flight.
        return true;
    }
    virtual void wait_and_process_events(const sigset_t* active_sigmask) override {
        _smp_wakeup_completion.maybe_rearm(*this);
        _hrtimer_completion.maybe_rearm(*this);
        ::io_uring_submit(&_uring);
        bool did_work = false;
        did_work |= _preempt_io_context.service_preempting_io();
        did_work |= std::exchange(_did_work_while_getting_sqe, false);
        if (did_work) {
            return;
        }
        struct ::io_uring_cqe* cqe = nullptr;
        sigset_t sigs = *active_sigmask; // io_uring_wait_cqes() wants non-const
        const auto before_wait_cqes = sched_clock::now();
        auto r = ::io_uring_wait_cqes(&_uring, &cqe, 1, nullptr, &sigs);
        _r._total_sleep += sched_clock::now() - before_wait_cqes;
        if (__builtin_expect(r < 0, false)) {
            switch (-r) {
            case EINTR:
                return;
            default:
                abort();
            }
        }
        did_work |= do_process_kernel_completions();
        _preempt_io_context.service_preempting_io();
    }
    virtual future<> readable(pollable_fd_state& fd) override {
        return poll(fd, POLLIN);
    }
    virtual future<> writeable(pollable_fd_state& fd) override {
        return poll(fd, POLLOUT);
    }
    virtual future<> readable_or_writeable(pollable_fd_state& fd) override {
        return poll(fd, POLLIN | POLLOUT);
    }
    virtual future<> poll_rdhup(pollable_fd_state& fd) override {
        return poll(fd, POLLRDHUP);
    }
    virtual void forget(pollable_fd_state& fd) noexcept override {
        auto* pfd = static_cast<uring_pollable_fd_state*>(&fd);
        delete pfd;
    }
    virtual future<std::tuple<pollable_fd, socket_address>> accept(pollable_fd_state& listenfd) override {
        if (listenfd.take_speculation(POLLIN)) {
            try {
                listenfd.maybe_no_more_recv();
                socket_address sa;
                auto maybe_fd = listenfd.fd.try_accept(sa, SOCK_CLOEXEC);
                if (maybe_fd) {
                    listenfd.speculate_epoll(EPOLLIN);
                    pollable_fd pfd(std::move(*maybe_fd), pollable_fd::speculation(EPOLLOUT));
                    return make_ready_future<std::tuple<pollable_fd, socket_address>>(std::move(pfd), std::move(sa));
                }
            } catch (...) {
                return current_exception_as_future<std::tuple<pollable_fd, socket_address>>();
            }
        }
        class accept_completion final : public io_completion {
            pollable_fd_state& _listenfd;
            socket_address _sa;
            promise<std::tuple<pollable_fd, socket_address>> _result;
        public:
            accept_completion(pollable_fd_state& listenfd)
                : _listenfd(listenfd) {}
            void complete(size_t fd) noexcept final {
                _listenfd.speculate_epoll(EPOLLIN);
                pollable_fd pfd(file_desc::from_fd(fd), pollable_fd::speculation(EPOLLOUT));
                _result.set_value(std::move(pfd), std::move(_sa));
                delete this;
            }
            void set_exception(std::exception_ptr eptr) noexcept final {
                try {
                    std::rethrow_exception(eptr);
                } catch (const std::system_error& e) {
                    if (e.code() == std::errc::invalid_argument) {
                        try {
                            // The chances are that we shutting down the connection.
                            _listenfd.maybe_no_more_recv();
                        } catch (...) {
                            eptr = std::current_exception();
                        }
                    }
                } catch (...) {}
                _result.set_exception(eptr);
                delete this;
            }
            future<std::tuple<pollable_fd, socket_address>> get_future() {
                return _result.get_future();
            }
            ::sockaddr* posix_sockaddr() {
                return &_sa.as_posix_sockaddr();
            }
            socklen_t* socklen_ptr() {
                return &_sa.addr_length;
            }
        };
        return readable_or_writeable(listenfd).then([this, &listenfd] {
            auto desc = std::make_unique<accept_completion>(listenfd);
            auto req = internal::io_request::make_accept(listenfd.fd.get(), desc->posix_sockaddr(), desc->socklen_ptr(), SOCK_NONBLOCK | SOCK_CLOEXEC);
            return submit_request(std::move(desc), std::move(req));
        });
    }
    virtual future<> connect(pollable_fd_state& fd, socket_address& sa) override {
        class connect_completion final : public io_completion {
            pollable_fd_state& _fd;
            socket_address _sa;
            promise<> _result;
        public:
            connect_completion(pollable_fd_state& fd, const socket_address& sa)
                : _fd(fd), _sa(sa) {}
            void complete(size_t fd) noexcept final {
                _fd.speculate_epoll(POLLOUT);
                _result.set_value();
                delete this;
            }
            void set_exception(std::exception_ptr eptr) noexcept final {
                _result.set_exception(eptr);
                delete this;
            }
            future<> get_future() {
                return _result.get_future();
            }
            ::sockaddr* posix_sockaddr() {
                return &_sa.as_posix_sockaddr();
            }
            socklen_t socklen() const {
                return _sa.addr_length;
            }
        };
        auto desc = std::make_unique<connect_completion>(fd, sa);
        auto req = internal::io_request::make_connect(fd.fd.get(), desc->posix_sockaddr(), desc->socklen());
        return submit_request(std::move(desc), std::move(req));
    }
    virtual void shutdown(pollable_fd_state& fd, int how) override {
        fd.fd.shutdown(how);
    }
    virtual future<size_t> read(pollable_fd_state& fd, void* buffer, size_t len) override {
        return _r.do_read(fd, buffer, len);
    }
    virtual future<size_t> recvmsg(pollable_fd_state& fd, const std::vector<iovec>& iov) override {
        if (fd.take_speculation(POLLIN)) {
            ::msghdr mh = {};
            mh.msg_iov = const_cast<iovec*>(iov.data());
            mh.msg_iovlen = iov.size();
            try {
                auto r = fd.fd.recvmsg(&mh, MSG_DONTWAIT);
                if (r) {
                    if (size_t(*r) == internal::iovec_len(iov)) {
                        fd.speculate_epoll(EPOLLIN);
                    }
                    return make_ready_future<size_t>(*r);
                }
            } catch (...) {
                return current_exception_as_future<size_t>();
            }
        }
        class read_completion final : public io_completion {
            pollable_fd_state& _fd;
            std::vector<iovec> _iov;
            ::msghdr _mh = {};
            promise<size_t> _result;
        public:
            read_completion(pollable_fd_state& fd, const std::vector<iovec>& iov)
                : _fd(fd), _iov(iov) {
                _mh.msg_iov = const_cast<iovec*>(_iov.data());
                _mh.msg_iovlen = _iov.size();
            }
            void complete(size_t bytes) noexcept final {
                if (bytes == internal::iovec_len(_iov)) {
                    _fd.speculate_epoll(EPOLLIN);
                }
                _result.set_value(bytes);
                delete this;
            }
            void set_exception(std::exception_ptr eptr) noexcept final {
                _result.set_exception(eptr);
                delete this;
            }
            ::msghdr* msghdr() {
                return &_mh;
            }
            future<size_t> get_future() {
                return _result.get_future();
            }
        };
        auto desc = std::make_unique<read_completion>(fd, iov);
        auto req = internal::io_request::make_recvmsg(fd.fd.get(), desc->msghdr(), 0);
        return submit_request(std::move(desc), std::move(req));
    }
    virtual future<temporary_buffer<char>> read_some(pollable_fd_state& fd, internal::buffer_allocator* ba) override {
        if (fd.take_speculation(POLLIN)) {
            auto buffer = ba->allocate_buffer();
            try {
                auto r = fd.fd.read(buffer.get_write(), buffer.size());
                if (r) {
                    if (size_t(*r) == buffer.size()) {
                        fd.speculate_epoll(EPOLLIN);
                    }
                    buffer.trim(*r);
                    return make_ready_future<temporary_buffer<char>>(std::move(buffer));
                }
            } catch (...) {
                return current_exception_as_future<temporary_buffer<char>>();
            }
        }
        return readable(fd).then([this, &fd, ba] {
            class read_completion final : public io_completion {
                pollable_fd_state& _fd;
                temporary_buffer<char> _buffer;
                promise<temporary_buffer<char>> _result;
            public:
                read_completion(pollable_fd_state& fd, temporary_buffer<char> buffer)
                    : _fd(fd), _buffer(std::move(buffer)) {}
                void complete(size_t bytes) noexcept final {
                    if (bytes == _buffer.size()) {
                        _fd.speculate_epoll(EPOLLIN);
                    }
                    _buffer.trim(bytes);
                    _result.set_value(std::move(_buffer));
                    delete this;
                }
                void set_exception(std::exception_ptr eptr) noexcept final {
                    _result.set_exception(eptr);
                    delete this;
                }
                future<temporary_buffer<char>> get_future() {
                    return _result.get_future();
                }
                char* get_write() {
                    return _buffer.get_write();
                }
                size_t get_size() {
                    return _buffer.size();
                }
            };
            auto desc = std::make_unique<read_completion>(fd, ba->allocate_buffer());
            auto req = internal::io_request::make_read(fd.fd.get(), -1, desc->get_write(), desc->get_size(), false);
            return submit_request(std::move(desc), std::move(req));
        });
    }
    virtual future<size_t> sendmsg(pollable_fd_state& fd, net::packet& p) final {
        if (fd.take_speculation(EPOLLOUT)) {
            static_assert(offsetof(iovec, iov_base) == offsetof(net::fragment, base) &&
                sizeof(iovec::iov_base) == sizeof(net::fragment::base) &&
                offsetof(iovec, iov_len) == offsetof(net::fragment, size) &&
                sizeof(iovec::iov_len) == sizeof(net::fragment::size) &&
                alignof(iovec) == alignof(net::fragment) &&
                sizeof(iovec) == sizeof(net::fragment)
                , "net::fragment and iovec should be equivalent");

            ::msghdr mh = {};
            mh.msg_iov = reinterpret_cast<iovec*>(p.fragment_array());
            mh.msg_iovlen = std::min<size_t>(p.nr_frags(), IOV_MAX);
            try {
                auto r = fd.fd.sendmsg(&mh, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (r) {
                    if (size_t(*r) == p.len()) {
                        fd.speculate_epoll(EPOLLOUT);
                    }
                    return make_ready_future<size_t>(*r);
                }
            } catch (...) {
                return current_exception_as_future<size_t>();
            }
        }
        class write_completion final : public io_completion {
            pollable_fd_state& _fd;
            ::msghdr _mh = {};
            const size_t _to_write;
            promise<size_t> _result;
        public:
            write_completion(pollable_fd_state& fd, net::packet& p)
                : _fd(fd), _to_write(p.len()) {
                _mh.msg_iov = reinterpret_cast<iovec*>(p.fragment_array());
                _mh.msg_iovlen = std::min<size_t>(p.nr_frags(), IOV_MAX);
            }
            void complete(size_t bytes) noexcept final {
                if (bytes == _to_write) {
                    _fd.speculate_epoll(EPOLLOUT);
                }
                _result.set_value(bytes);
                delete this;
            }
            void set_exception(std::exception_ptr eptr) noexcept final {
                _result.set_exception(eptr);
                delete this;
            }
            ::msghdr* msghdr() {
                return &_mh;
            }
            future<size_t> get_future() {
                return _result.get_future();
            }
        };
        auto desc = std::make_unique<write_completion>(fd, p);
        auto req = internal::io_request::make_sendmsg(fd.fd.get(), desc->msghdr(), MSG_NOSIGNAL);
        return submit_request(std::move(desc), std::move(req));
    }
    virtual future<size_t> send(pollable_fd_state& fd, const void* buffer, size_t len) override {
        if (fd.take_speculation(EPOLLOUT)) {
            try {
                auto r = fd.fd.send(buffer, len, MSG_NOSIGNAL | MSG_DONTWAIT);
                if (r) {
                    if (size_t(*r) == len) {
                        fd.speculate_epoll(EPOLLOUT);
                    }
                    return make_ready_future<size_t>(*r);
                }
            } catch (...) {
                return current_exception_as_future<size_t>();
            }
        }
        class write_completion final : public io_completion {
            pollable_fd_state& _fd;
            const size_t _to_write;
            promise<size_t> _result;
        public:
            write_completion(pollable_fd_state& fd, size_t to_write)
                : _fd(fd), _to_write(to_write) {}
            void complete(size_t bytes) noexcept final {
                if (bytes == _to_write) {
                    _fd.speculate_epoll(EPOLLOUT);
                }
                _result.set_value(bytes);
                delete this;
            }
            void set_exception(std::exception_ptr eptr) noexcept final {
                _result.set_exception(eptr);
                delete this;
            }
            future<size_t> get_future() {
                return _result.get_future();
            }
        };
        auto desc = std::make_unique<write_completion>(fd, len);
        auto req = internal::io_request::make_send(fd.fd.get(), buffer, len, MSG_NOSIGNAL);
        return submit_request(std::move(desc), std::move(req));
    }

    virtual future<temporary_buffer<char>> recv_some(pollable_fd_state& fd, internal::buffer_allocator* ba) override {
        if (fd.take_speculation(POLLIN)) {
            auto buffer = ba->allocate_buffer();
            try {
                auto r = fd.fd.recv(buffer.get_write(), buffer.size(), MSG_DONTWAIT);
                if (r) {
                    if (size_t(*r) == buffer.size()) {
                        fd.speculate_epoll(EPOLLIN);
                    }
                    buffer.trim(*r);
                    return make_ready_future<temporary_buffer<char>>(std::move(buffer));
                }
            } catch (...) {
                return current_exception_as_future<temporary_buffer<char>>();
            }
        }
        class recv_completion final : public io_completion {
            pollable_fd_state& _fd;
            temporary_buffer<char> _buffer;
            promise<temporary_buffer<char>> _result;
        public:
            recv_completion(pollable_fd_state& fd, temporary_buffer<char> buffer)
                : _fd(fd), _buffer(std::move(buffer)) {}
            void complete(size_t bytes) noexcept final {
                if (bytes == _buffer.size()) {
                    _fd.speculate_epoll(EPOLLIN);
                }
                _buffer.trim(bytes);
                _result.set_value(std::move(_buffer));
                delete this;
            }
            void set_exception(std::exception_ptr eptr) noexcept final {
                _result.set_exception(eptr);
                delete this;
            }
            future<temporary_buffer<char>> get_future() {
                return _result.get_future();
            }
            char* get_write() {
                return _buffer.get_write();
            }
            size_t get_size() {
                return _buffer.size();
            }
        };
        auto desc = std::make_unique<recv_completion>(fd, ba->allocate_buffer());
        auto req = internal::io_request::make_recv(fd.fd.get(), desc->get_write(), desc->get_size(), 0);
        return submit_request(std::move(desc), std::move(req));
    }

    virtual bool do_blocking_io() const override {
        return true;
    }

    virtual void signal_received(int signo, siginfo_t* siginfo, void* ignore) override {
        _r._signals.action(signo, siginfo, ignore);
    }
    virtual void start_tick() override {
        _preempt_io_context.start_tick();
    }
    virtual void stop_tick() override {
        _preempt_io_context.stop_tick();
    }
    virtual void arm_highres_timer(const ::itimerspec& its) override {
        _hrtimer_timerfd.timerfd_settime(TFD_TIMER_ABSTIME, its);
    }
    virtual void reset_preemption_monitor() override {
        _preempt_io_context.reset_preemption_monitor();
    }
    virtual void request_preemption() override {
        _preempt_io_context.request_preemption();
    }
    virtual void start_handling_signal() override {
        // We don't have anything special wrt. signals
    }
    virtual pollable_fd_state_ptr make_pollable_fd_state(file_desc fd, pollable_fd::speculation speculate) override {
        return pollable_fd_state_ptr(new uring_pollable_fd_state(std::move(fd), std::move(speculate)));
    }
};

#endif

static bool detect_aio_poll() {
    auto fd = file_desc::eventfd(0, 0);
    aio_context_t ioc{};
    setup_aio_context(1, &ioc);
    auto cleanup = defer([&] () noexcept { io_destroy(ioc); });
    linux_abi::iocb iocb = internal::make_poll_iocb(fd.get(), POLLIN|POLLOUT);
    linux_abi::iocb* a[1] = { &iocb };
    auto r = io_submit(ioc, 1, a);
    if (r != 1) {
        return false;
    }
    uint64_t one = 1;
    fd.write(&one, 8);
    io_event ev[1];
    // We set force_syscall = true (the last parameter) to ensure
    // the system call exists and is usable. If IOCB_CMD_POLL exists then
    // io_pgetevents() will also exist, but some versions of docker
    // have a syscall whitelist that does not include io_pgetevents(),
    // which causes it to fail with -EPERM. See
    // https://github.com/moby/moby/issues/38894.
    r = io_pgetevents(ioc, 1, 1, ev, nullptr, nullptr, true);
    return r == 1;
}

bool reactor_backend_selector::has_enough_aio_nr() {
    auto aio_max_nr = read_first_line_as<unsigned>("/proc/sys/fs/aio-max-nr");
    auto aio_nr = read_first_line_as<unsigned>("/proc/sys/fs/aio-nr");
    /* reactor_backend_selector::available() will be execute in early stage,
     * it's before io_setup() issued, and not per-cpu basis.
     * So this method calculates:
     *  Available AIO on the system - (request AIO per-cpu * ncpus)
     */
    if (aio_max_nr - aio_nr < reactor::max_aio * smp::count) {
        return false;
    }
    return true;
}

std::unique_ptr<reactor_backend> reactor_backend_selector::create(reactor& r) {
    if (_name == "io_uring") {
#ifdef SEASTAR_HAVE_URING
        return std::make_unique<reactor_backend_uring>(r);
#else
        throw std::runtime_error("io_uring backend not compiled in");
#endif
    }
    if (_name == "linux-aio") {
        return std::make_unique<reactor_backend_aio>(r);
    } else if (_name == "epoll") {
        return std::make_unique<reactor_backend_epoll>(r);
    }
    throw std::logic_error("bad reactor backend");
}

reactor_backend_selector reactor_backend_selector::default_backend() {
    return available()[0];
}

std::vector<reactor_backend_selector> reactor_backend_selector::available() {
    std::vector<reactor_backend_selector> ret;
#ifdef SEASTAR_HAVE_URING
    if (detect_io_uring()) {
        ret.push_back(reactor_backend_selector("io_uring"));
    }
#endif
    if (has_enough_aio_nr() && detect_aio_poll()) {
        ret.push_back(reactor_backend_selector("linux-aio"));
    }
    ret.push_back(reactor_backend_selector("epoll"));
    return ret;
}

}
