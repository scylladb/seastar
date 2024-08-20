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
#include <seastar/core/internal/io_desc.hh>
#include <seastar/core/internal/pollable_fd.hh>
#include <seastar/core/internal/poll.hh>
#include <seastar/core/linux-aio.hh>
#include <seastar/core/cacheline.hh>
#include <seastar/util/modules.hh>

#ifndef SEASTAR_MODULE
#include <fmt/ostream.h>
#include <sys/time.h>
#include <thread>
#include <stack>
#include <boost/any.hpp>
#include <boost/program_options.hpp>
#include <boost/container/static_vector.hpp>

#endif

namespace seastar {

class reactor;

// FIXME: merge it with storage context below. At this point the
// main thing to do is unify the iocb list
struct aio_general_context {
    explicit aio_general_context(size_t nr);
    ~aio_general_context();
    internal::linux_abi::aio_context_t io_context{};
    std::unique_ptr<internal::linux_abi::iocb*[]> iocbs;
    internal::linux_abi::iocb** last;
    internal::linux_abi::iocb** const end;
    void queue(internal::linux_abi::iocb* iocb);
    // submit all queued iocbs and return their count.
    size_t flush();
};

class aio_storage_context {
    static constexpr unsigned max_aio = 1024;

    class iocb_pool {
        alignas(cache_line_size) std::array<internal::linux_abi::iocb, max_aio> _iocb_pool;
        std::stack<internal::linux_abi::iocb*, boost::container::static_vector<internal::linux_abi::iocb*, max_aio>> _free_iocbs;
    public:
        iocb_pool();
        internal::linux_abi::iocb& get_one();
        void put_one(internal::linux_abi::iocb* io);
        unsigned outstanding() const;
        bool has_capacity() const;
    };

    reactor& _r;
    internal::linux_abi::aio_context_t _io_context;
    boost::container::static_vector<internal::linux_abi::iocb*, max_aio> _submission_queue;
    iocb_pool _iocb_pool;
    size_t handle_aio_error(internal::linux_abi::iocb* iocb, int ec);
    using pending_aio_retry_t = boost::container::static_vector<internal::linux_abi::iocb*, max_aio>;
    pending_aio_retry_t _pending_aio_retry; // Pending retries iocbs
    pending_aio_retry_t _aio_retries;       // Currently retried iocbs
    future<> _pending_aio_retry_fut = make_ready_future<>();
    internal::linux_abi::io_event _ev_buffer[max_aio];

    bool need_to_retry() const noexcept {
        return !_pending_aio_retry.empty() || !_aio_retries.empty();
    }

    bool retry_in_progress() const noexcept {
        return !_pending_aio_retry_fut.available();
    }

public:
    explicit aio_storage_context(reactor& r);
    ~aio_storage_context();

    bool reap_completions(bool allow_retry = true);
    void schedule_retry();
    bool submit_work();
    bool can_sleep() const;
    future<> stop() noexcept;
};

class completion_with_iocb {
    bool _in_context = false;
    internal::linux_abi::iocb _iocb;
protected:
    completion_with_iocb(int fd, int events, void* user_data);
    void completed() {
        _in_context = false;
    }
public:
    void maybe_queue(aio_general_context& context);
};

class fd_kernel_completion : public kernel_completion {
protected:
    file_desc& _fd;
    fd_kernel_completion(file_desc& fd) : _fd(fd) {}
public:
    file_desc& fd() {
        return _fd;
    }
};

struct hrtimer_aio_completion : public fd_kernel_completion,
                                public completion_with_iocb {
private:
    reactor& _r;
public:
    hrtimer_aio_completion(reactor& r, file_desc& fd);
    virtual void complete_with(ssize_t value) override;
};

struct task_quota_aio_completion : public fd_kernel_completion,
                                   public completion_with_iocb {
    task_quota_aio_completion(file_desc& fd);
    virtual void complete_with(ssize_t value) override;
};

struct smp_wakeup_aio_completion : public fd_kernel_completion,
                                   public completion_with_iocb {
    smp_wakeup_aio_completion(file_desc& fd);
    virtual void complete_with(ssize_t value) override;
};

// Common aio-based Implementation of the task quota and hrtimer.
class preempt_io_context {
    reactor& _r;
    aio_general_context _context{2};

    task_quota_aio_completion _task_quota_aio_completion;
    hrtimer_aio_completion _hrtimer_aio_completion;
public:
    preempt_io_context(reactor& r, file_desc& task_quota, file_desc& hrtimer);
    bool service_preempting_io();

    size_t flush() {
        return _context.flush();
    }

    void reset_preemption_monitor();
    void request_preemption();
    void start_tick();
    void stop_tick();
};

// The "reactor_backend" interface provides a method of waiting for various
// basic events on one thread. We have one implementation based on epoll and
// file-descriptors (reactor_backend_epoll), one implementation based on
// linux aio, and one implementation based on io_uring.
class reactor_backend {
public:
    virtual ~reactor_backend() {};
    // The methods below are used to communicate with the kernel.
    // reap_kernel_completions() will complete any previous async
    // work that is ready to consume.
    // kernel_submit_work() submit new events that were produced.
    // Both of those methods are asynchronous and will never block.
    //
    // wait_and_process_events on the other hand may block, and is called when
    // we are about to go to sleep.
    virtual bool reap_kernel_completions() = 0;
    virtual bool kernel_submit_work() = 0;
    virtual bool kernel_events_can_sleep() const = 0;
    virtual void wait_and_process_events(const sigset_t* active_sigmask = nullptr) = 0;

    // Methods that allow polling on file descriptors. This will only work on
    // reactor_backend_epoll. Other reactor_backend will probably abort if
    // they are called (which is fine if no file descriptors are waited on):
    virtual future<> readable(pollable_fd_state& fd) = 0;
    virtual future<> writeable(pollable_fd_state& fd) = 0;
    virtual future<> readable_or_writeable(pollable_fd_state& fd) = 0;
    virtual future<> poll_rdhup(pollable_fd_state& fd) = 0;
    virtual void forget(pollable_fd_state& fd) noexcept = 0;

    virtual future<std::tuple<pollable_fd, socket_address>>
    accept(pollable_fd_state& listenfd) = 0;
    virtual future<> connect(pollable_fd_state& fd, socket_address& sa) = 0;
    virtual void shutdown(pollable_fd_state& fd, int how) = 0;
    virtual future<size_t> read(pollable_fd_state& fd, void* buffer, size_t len) = 0;
    virtual future<size_t> recvmsg(pollable_fd_state& fd, const std::vector<iovec>& iov) = 0;
    virtual future<temporary_buffer<char>> read_some(pollable_fd_state& fd, internal::buffer_allocator* ba) = 0;
    virtual future<size_t> sendmsg(pollable_fd_state& fd, net::packet& p) = 0;
    virtual future<size_t> send(pollable_fd_state& fd, const void* buffer, size_t len) = 0;
    virtual future<temporary_buffer<char>> recv_some(pollable_fd_state& fd, internal::buffer_allocator* ba) = 0;

    virtual bool do_blocking_io() const {
        return false;
    }
    virtual void signal_received(int signo, siginfo_t* siginfo, void* ignore) = 0;
    virtual void start_tick() = 0;
    virtual void stop_tick() = 0;
    virtual void arm_highres_timer(const ::itimerspec& ts) = 0;
    virtual void reset_preemption_monitor() = 0;
    virtual void request_preemption() = 0;
    virtual void start_handling_signal() = 0;

    virtual pollable_fd_state_ptr make_pollable_fd_state(file_desc fd, pollable_fd::speculation speculate) = 0;
};

// reactor backend using file-descriptor & epoll, suitable for running on
// Linux. Can wait on multiple file descriptors, and converts other events
// (such as timers, signals, inter-thread notifications) into file descriptors
// using mechanisms like timerfd, signalfd and eventfd respectively.
class reactor_backend_epoll : public reactor_backend {
    reactor& _r;
    std::atomic<bool> _highres_timer_pending = {};
    std::thread _task_quota_timer_thread;
    ::itimerspec _steady_clock_timer_deadline = {};
    // These two timers are used for high resolution timer<>s, one for
    // the reactor thread (when sleeping) and one for the timer thread
    // (when awake). We can't use one timer because of races between the
    // timer thread and reactor thread.
    //
    // Only one of the two is active at any time.
    file_desc _steady_clock_timer_reactor_thread;
    file_desc _steady_clock_timer_timer_thread;
private:
    file_desc _epollfd;
    void task_quota_timer_thread_fn();
    future<> get_epoll_future(pollable_fd_state& fd, int event);
    void complete_epoll_event(pollable_fd_state& fd, int events, int event);
    aio_storage_context _storage_context;
    void switch_steady_clock_timers(file_desc& from, file_desc& to);
    void maybe_switch_steady_clock_timers(int timeout, file_desc& from, file_desc& to);
    bool wait_and_process(int timeout, const sigset_t* active_sigmask);
    bool complete_hrtimer();
    bool _need_epoll_events = false;
public:
    explicit reactor_backend_epoll(reactor& r);
    virtual ~reactor_backend_epoll() override;

    virtual bool reap_kernel_completions() override;
    virtual bool kernel_submit_work() override;
    virtual bool kernel_events_can_sleep() const override;
    virtual void wait_and_process_events(const sigset_t* active_sigmask) override;
    virtual future<> readable(pollable_fd_state& fd) override;
    virtual future<> writeable(pollable_fd_state& fd) override;
    virtual future<> readable_or_writeable(pollable_fd_state& fd) override;
    virtual future<> poll_rdhup(pollable_fd_state& fd) override;
    virtual void forget(pollable_fd_state& fd) noexcept override;

    virtual future<std::tuple<pollable_fd, socket_address>>
    accept(pollable_fd_state& listenfd) override;
    virtual future<> connect(pollable_fd_state& fd, socket_address& sa) override;
    virtual void shutdown(pollable_fd_state& fd, int how) override;
    virtual future<size_t> read(pollable_fd_state& fd, void* buffer, size_t len) override;
    virtual future<size_t> recvmsg(pollable_fd_state& fd, const std::vector<iovec>& iov) override;
    virtual future<temporary_buffer<char>> read_some(pollable_fd_state& fd, internal::buffer_allocator* ba) override;
    virtual future<size_t> sendmsg(pollable_fd_state& fd, net::packet& p) override;
    virtual future<size_t> send(pollable_fd_state& fd, const void* buffer, size_t len) override;
    virtual future<temporary_buffer<char>> recv_some(pollable_fd_state& fd, internal::buffer_allocator* ba) override;

    virtual void signal_received(int signo, siginfo_t* siginfo, void* ignore) override;
    virtual void start_tick() override;
    virtual void stop_tick() override;
    virtual void arm_highres_timer(const ::itimerspec& ts) override;
    virtual void reset_preemption_monitor() override;
    virtual void request_preemption() override;
    virtual void start_handling_signal() override;

    virtual pollable_fd_state_ptr
    make_pollable_fd_state(file_desc fd, pollable_fd::speculation speculate) override;
};

class reactor_backend_aio : public reactor_backend {
    reactor& _r;
    file_desc _hrtimer_timerfd;
    aio_storage_context _storage_context;
    // We use two aio contexts, one for preempting events (the timer tick and
    // signals), the other for non-preempting events (fd poll).
    preempt_io_context _preempting_io; // Used for the timer tick and the high resolution timer
    aio_general_context _polling_io; // FIXME: unify with disk aio_context
    hrtimer_aio_completion _hrtimer_poll_completion;
    smp_wakeup_aio_completion _smp_wakeup_aio_completion;
    static file_desc make_timerfd();
    bool await_events(int timeout, const sigset_t* active_sigmask);
    future<> poll(pollable_fd_state& fd, int events);
public:
    explicit reactor_backend_aio(reactor& r);

    virtual bool reap_kernel_completions() override;
    virtual bool kernel_submit_work() override;
    virtual bool kernel_events_can_sleep() const override;
    virtual void wait_and_process_events(const sigset_t* active_sigmask) override;
    virtual future<> readable(pollable_fd_state& fd) override;
    virtual future<> writeable(pollable_fd_state& fd) override;
    virtual future<> readable_or_writeable(pollable_fd_state& fd) override;
    virtual future<> poll_rdhup(pollable_fd_state& fd) override;
    virtual void forget(pollable_fd_state& fd) noexcept override;

    virtual future<std::tuple<pollable_fd, socket_address>>
    accept(pollable_fd_state& listenfd) override;
    virtual future<> connect(pollable_fd_state& fd, socket_address& sa) override;
    virtual void shutdown(pollable_fd_state& fd, int how) override;
    virtual future<size_t> read(pollable_fd_state& fd, void* buffer, size_t len) override;
    virtual future<size_t> recvmsg(pollable_fd_state& fd, const std::vector<iovec>& iov) override;
    virtual future<temporary_buffer<char>> read_some(pollable_fd_state& fd, internal::buffer_allocator* ba) override;
    virtual future<size_t> sendmsg(pollable_fd_state& fd, net::packet& p) override;
    virtual future<size_t> send(pollable_fd_state& fd, const void* buffer, size_t len) override;
    virtual future<temporary_buffer<char>> recv_some(pollable_fd_state& fd, internal::buffer_allocator* ba) override;

    virtual void signal_received(int signo, siginfo_t* siginfo, void* ignore) override;
    virtual void start_tick() override;
    virtual void stop_tick() override;
    virtual void arm_highres_timer(const ::itimerspec& its) override;
    virtual void reset_preemption_monitor() override;
    virtual void request_preemption() override;
    virtual void start_handling_signal() override;

    virtual pollable_fd_state_ptr
    make_pollable_fd_state(file_desc fd, pollable_fd::speculation speculate) override;
};

class reactor_backend_uring;

class reactor_backend_selector {
    std::string _name;
private:
    static bool has_enough_aio_nr();
    explicit reactor_backend_selector(std::string name) : _name(std::move(name)) {}
public:
    const std::string& name() const { return _name; }
    std::unique_ptr<reactor_backend> create(reactor& r);
    static reactor_backend_selector default_backend();
    static std::vector<reactor_backend_selector> available();
    friend std::ostream& operator<<(std::ostream& os, const reactor_backend_selector& rbs) {
        return os << rbs._name;
    }
};

}

#if FMT_VERSION >= 90000

template <> struct fmt::formatter<seastar::reactor_backend_selector> : fmt::ostream_formatter {};

#endif
