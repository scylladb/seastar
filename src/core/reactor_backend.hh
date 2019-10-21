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

#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/internal/pollable_fd.hh>
#include <seastar/core/internal/poll.hh>
#include <seastar/core/linux-aio.hh>
#include <sys/time.h>
#include <signal.h>
#include <thread>
#include <stack>
#include <boost/any.hpp>
#include <boost/program_options.hpp>

#ifdef HAVE_OSV
#include <osv/newpoll.hh>
#endif

namespace seastar {

class reactor;

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
    virtual void start_handling_signal() = 0;
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
    virtual void start_handling_signal() override;
};

class reactor_backend_aio : public reactor_backend {
    static constexpr size_t max_polls = 10000;
    reactor* _r;
    // We use two aio contexts, one for preempting events (the timer tick and
    // signals), the other for non-preempting events (fd poll).
    struct context {
        explicit context(size_t nr);
        ~context();
        internal::linux_abi::aio_context_t io_context{};
        std::unique_ptr<internal::linux_abi::iocb*[]> iocbs;
        internal::linux_abi::iocb** last = iocbs.get();
        void replenish(internal::linux_abi::iocb* iocb, bool& flag);
        void queue(internal::linux_abi::iocb* iocb);
        void flush();
    };
    context _preempting_io{2}; // Used for the timer tick and the high resolution timer
    context _polling_io{max_polls}; // FIXME: unify with disk aio_context
    file_desc _steady_clock_timer = make_timerfd();
    internal::linux_abi::iocb _task_quota_timer_iocb;
    internal::linux_abi::iocb _timerfd_iocb;
    internal::linux_abi::iocb _smp_wakeup_iocb;
    bool _task_quota_timer_in_preempting_io = false;
    bool _timerfd_in_preempting_io = false;
    bool _timerfd_in_polling_io = false;
    bool _smp_wakeup_in_polling_io = false;
    std::stack<std::unique_ptr<internal::linux_abi::iocb>> _iocb_pool;
private:
    internal::linux_abi::iocb* new_iocb();
    void free_iocb(internal::linux_abi::iocb* iocb);
    static file_desc make_timerfd();
    void process_task_quota_timer();
    void process_timerfd();
    void process_smp_wakeup();
    bool service_preempting_io();
    bool await_events(int timeout, const sigset_t* active_sigmask);
    static void signal_received(int signo, siginfo_t* siginfo, void* ignore);
private:
    class io_poll_poller : public seastar::pollfn {
        reactor_backend_aio* _backend;
    public:
        explicit io_poll_poller(reactor_backend_aio* b);
        virtual bool poll() override;
        virtual bool pure_poll() override;
        virtual bool try_enter_interrupt_mode() override;
        virtual void exit_interrupt_mode() override;
    };
public:
    explicit reactor_backend_aio(reactor* r);
    virtual bool wait_and_process(int timeout, const sigset_t* active_sigmask) override;
    future<> poll(pollable_fd_state& fd, promise<> pollable_fd_state::*promise_field, int events);
    virtual future<> readable(pollable_fd_state& fd) override;
    virtual future<> writeable(pollable_fd_state& fd) override;
    virtual future<> readable_or_writeable(pollable_fd_state& fd) override;
    virtual void forget(pollable_fd_state& fd) override;
    virtual void handle_signal(int signo) override;
    virtual void start_tick() override;
    virtual void stop_tick() override;
    virtual void arm_highres_timer(const ::itimerspec& its) override;
    virtual void reset_preemption_monitor() override;
    virtual void request_preemption() override;
    virtual void start_handling_signal() override;
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

class reactor_backend_selector {
    std::string _name;
private:
    explicit reactor_backend_selector(std::string name) : _name(std::move(name)) {}
public:
    std::unique_ptr<reactor_backend> create(reactor* r);
    static reactor_backend_selector default_backend();
    static std::vector<reactor_backend_selector> available();
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

}
