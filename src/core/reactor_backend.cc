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
#include "core/reactor_backend.hh"
#include <seastar/core/print.hh>
#include <seastar/core/reactor.hh>
#include <chrono>
#include <sys/poll.h>

namespace seastar {

using namespace std::chrono_literals;
using namespace internal;
using namespace internal::linux_abi;

reactor_backend_aio::context::context(size_t nr) : iocbs(new iocb*[nr]) {
    setup_aio_context(nr, &io_context);
}

reactor_backend_aio::context::~context() {
    io_destroy(io_context);
}

void reactor_backend_aio::context::replenish(linux_abi::iocb* iocb, bool& flag) {
    if (!flag) {
        flag = true;
        queue(iocb);
    }
}

void reactor_backend_aio::context::queue(linux_abi::iocb* iocb) {
    *last++ = iocb;
}

void reactor_backend_aio::context::flush() {
    if (last != iocbs.get()) {
        auto nr = last - iocbs.get();
        last = iocbs.get();
        io_submit(io_context, nr, iocbs.get());
    }
}

reactor_backend_aio::io_poll_poller::io_poll_poller(reactor_backend_aio* b) : _backend(b) {
}

bool reactor_backend_aio::io_poll_poller::poll() {
    return _backend->wait_and_process(0, nullptr);
}

bool reactor_backend_aio::io_poll_poller::pure_poll() {
    return _backend->wait_and_process(0, nullptr);
}

bool reactor_backend_aio::io_poll_poller::try_enter_interrupt_mode() {
    return true;
}

void reactor_backend_aio::io_poll_poller::exit_interrupt_mode() {
}

linux_abi::iocb* reactor_backend_aio::new_iocb() {
    if (_iocb_pool.empty()) {
        return new linux_abi::iocb;
    }
    auto ret = _iocb_pool.top().release();
    _iocb_pool.pop();
    return ret;
}

void reactor_backend_aio::free_iocb(linux_abi::iocb* iocb) {
    _iocb_pool.push(std::unique_ptr<linux_abi::iocb>(iocb));
}

file_desc reactor_backend_aio::make_timerfd() {
    return file_desc::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC|TFD_NONBLOCK);
}

void reactor_backend_aio::process_task_quota_timer() {
    uint64_t v;
    (void)_r->_task_quota_timer.read(&v, 8);
}

void reactor_backend_aio::process_timerfd() {
    uint64_t expirations = 0;
    _steady_clock_timer.read(&expirations, 8);
    if (expirations) {
        _r->service_highres_timer();
    }
}

void reactor_backend_aio::process_smp_wakeup() {
    uint64_t ignore = 0;
    _r->_notify_eventfd.read(&ignore, 8);
}

bool reactor_backend_aio::service_preempting_io() {
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

void reactor_backend_aio::signal_received(int signo, siginfo_t* siginfo, void* ignore) {
    engine()._signals.action(signo, siginfo, ignore);
}

reactor_backend_aio::reactor_backend_aio(reactor* r) : _r(r) {
    _task_quota_timer_iocb = make_poll_iocb(_r->_task_quota_timer.get(), POLLIN);
    _timerfd_iocb = make_poll_iocb(_steady_clock_timer.get(), POLLIN);
    _smp_wakeup_iocb = make_poll_iocb(_r->_notify_eventfd.get(), POLLIN);
    // Protect against spurious wakeups - if we get notified that the timer has
    // expired when it really hasn't, we don't want to block in read(tfd, ...).
    auto tfd = _r->_task_quota_timer.get();
    ::fcntl(tfd, F_SETFL, ::fcntl(tfd, F_GETFL) | O_NONBLOCK);
}

bool reactor_backend_aio::wait_and_process(int timeout, const sigset_t* active_sigmask) {
    bool did_work = service_preempting_io();
    if (did_work) {
        timeout = 0;
    }
    _polling_io.replenish(&_timerfd_iocb, _timerfd_in_polling_io);
    _polling_io.replenish(&_smp_wakeup_iocb, _smp_wakeup_in_polling_io);
    _polling_io.flush();
    did_work |= await_events(timeout, active_sigmask);
    did_work |= service_preempting_io(); // clear task quota timer
    return did_work;
}

future<> reactor_backend_aio::poll(pollable_fd_state& fd, promise<> pollable_fd_state::*promise_field, int events) {
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

future<> reactor_backend_aio::readable(pollable_fd_state& fd) {
    return poll(fd, &pollable_fd_state::pollin, POLLIN);
}

future<> reactor_backend_aio::writeable(pollable_fd_state& fd) {
    return poll(fd, &pollable_fd_state::pollout, POLLOUT);
}

future<> reactor_backend_aio::readable_or_writeable(pollable_fd_state& fd) {
    return poll(fd, &pollable_fd_state::pollin, POLLIN|POLLOUT);
}

void reactor_backend_aio::forget(pollable_fd_state& fd) {
    // ?
}

void reactor_backend_aio::handle_signal(int signo) {
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

void reactor_backend_aio::start_tick() {
    // Preempt whenever an event (timer tick or signal) is available on the
    // _preempting_io ring
    g_need_preempt = reinterpret_cast<const preemption_monitor*>(_preempting_io.io_context + 8);
    // reactor::request_preemption() will write to reactor::_preemption_monitor, which is now ignored
}

void reactor_backend_aio::stop_tick() {
    g_need_preempt = &_r->_preemption_monitor;
}

void reactor_backend_aio::arm_highres_timer(const ::itimerspec& its) {
    _steady_clock_timer.timerfd_settime(TFD_TIMER_ABSTIME, its);
}

void reactor_backend_aio::reset_preemption_monitor() {
    service_preempting_io();
    _preempting_io.replenish(&_timerfd_iocb, _timerfd_in_preempting_io);
    _preempting_io.replenish(&_task_quota_timer_iocb, _task_quota_timer_in_preempting_io);
    _preempting_io.flush();
}

void reactor_backend_aio::request_preemption() {
    ::itimerspec expired = {};
    expired.it_value.tv_nsec = 1;
    arm_highres_timer(expired); // will trigger immediately, triggering the preemption monitor

    // This might have been called from poll_once. If that is the case, we cannot assume that timerfd is being
    // monitored.
    _preempting_io.replenish(&_timerfd_iocb, _timerfd_in_preempting_io);
    _preempting_io.flush();

    // The kernel is not obliged to deliver the completion immediately, so wait for it
    while (!need_preempt()) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
}

void reactor_backend_aio::start_handling_signal() {
    // The aio backend only uses SIGHUP/SIGTERM/SIGINT. We don't need to handle them right away and our
    // implementation of request_preemption is not signal safe, so do nothing.
}

}
