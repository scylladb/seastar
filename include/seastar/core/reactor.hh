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

#pragma once

#include <seastar/core/aligned_buffer.hh>
#include <seastar/core/cacheline.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/core/circular_buffer_fixed_capacity.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/enum.hh>
#include <seastar/core/file.hh>
#include <seastar/core/future.hh>
#include <seastar/core/idle_cpu_handler.hh>
#include <seastar/core/internal/io_desc.hh>
#include <seastar/core/internal/io_request.hh>
#include <seastar/core/internal/io_sink.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/make_task.hh>
#include <seastar/core/manual_clock.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/core/internal/estimated_histogram.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/reactor_config.hh>
#include <seastar/core/scattered_message.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/scheduling_specific.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/shared_mutex.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/thread_cputime_clock.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/gate.hh>
#include <seastar/net/api.hh>
#include <seastar/util/eclipse.hh>
#include <seastar/util/log.hh>
#include <seastar/util/modules.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/util/std-compat.hh>
#include "internal/pollable_fd.hh"

#ifndef SEASTAR_MODULE
#include <boost/container/static_vector.hpp>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>

#endif

struct statfs;
struct _Unwind_Exception;

namespace seastar {

using shard_id = unsigned;

namespace alien {
class instance;
}
SEASTAR_MODULE_EXPORT
class reactor;

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

class thread_pool;
class smp;

class reactor_backend_selector;

class reactor_backend;
struct pollfn;

namespace internal {

class reactor_stall_sampler;
class cpu_stall_detector;
class buffer_allocator;
class priority_class;
class poller;

size_t scheduling_group_count();

void increase_thrown_exceptions_counter() noexcept;

}

class io_queue;
SEASTAR_MODULE_EXPORT
class io_intent;

class io_completion : public kernel_completion {
public:
    virtual void complete_with(ssize_t res) final override;

    virtual void complete(size_t res) noexcept = 0;
    virtual void set_exception(std::exception_ptr eptr) noexcept = 0;
};

SEASTAR_MODULE_EXPORT
class reactor {
private:
    struct task_queue;
    using task_queue_list = circular_buffer_fixed_capacity<task_queue*, 1 << log2ceil(max_scheduling_groups())>;
    using pollfn = seastar::pollfn;

    class signal_pollfn;
    class batch_flush_pollfn;
    class smp_pollfn;
    class drain_cross_cpu_freelist_pollfn;
    class lowres_timer_pollfn;
    class manual_timer_pollfn;
    class epoll_pollfn;
    class reap_kernel_completions_pollfn;
    class kernel_submit_work_pollfn;
    class io_queue_submission_pollfn;
    class syscall_pollfn;
    class execution_stage_pollfn;
    friend class manual_clock;
    friend class file_data_source_impl; // for fstream statistics
    friend class internal::reactor_stall_sampler;
    friend class preempt_io_context;
    friend struct hrtimer_aio_completion;
    friend class reactor_backend_epoll;
    friend class reactor_backend_aio;
    friend class reactor_backend_uring;
    friend class reactor_backend_selector;
    friend class io_queue; // for aio statistics
    friend struct reactor_options;
    friend class aio_storage_context;
public:
    using poller = internal::poller;
    using idle_cpu_handler_result = seastar::idle_cpu_handler_result;
    using work_waiting_on_reactor = seastar::work_waiting_on_reactor;
    using idle_cpu_handler = seastar::idle_cpu_handler;

    struct io_stats {
        uint64_t aio_reads = 0;
        uint64_t aio_read_bytes = 0;
        uint64_t aio_writes = 0;
        uint64_t aio_write_bytes = 0;
        uint64_t aio_outsizes = 0;
        uint64_t aio_errors = 0;
        uint64_t fstream_reads = 0;
        uint64_t fstream_read_bytes = 0;
        uint64_t fstream_reads_blocked = 0;
        uint64_t fstream_read_bytes_blocked = 0;
        uint64_t fstream_read_aheads_discarded = 0;
        uint64_t fstream_read_ahead_discarded_bytes = 0;
    };
    /// Scheduling statistics.
    struct sched_stats {
        /// Total number of tasks processed by this shard's reactor until this point.
        /// Note that tasks can be tiny, running for a few nanoseconds, or can take an
        /// entire task quota.
        uint64_t tasks_processed = 0;
    };
    friend void io_completion::complete_with(ssize_t);

    /// Obtains an alien::instance object that can be used to send messages
    /// to Seastar shards from non-Seastar threads.
    alien::instance& alien() { return _alien; }

private:
    std::shared_ptr<smp> _smp;
    alien::instance& _alien;
    reactor_config _cfg;
    file_desc _notify_eventfd;
    file_desc _task_quota_timer;
    std::unique_ptr<reactor_backend> _backend;
    sigset_t _active_sigmask; // holds sigmask while sleeping with sig disabled
    std::vector<pollfn*> _pollers;

    static constexpr unsigned max_aio_per_queue = 128;
    static constexpr unsigned max_queues = 8;
    static constexpr unsigned max_aio = max_aio_per_queue * max_queues;

    // Each mountpouint is controlled by its own io_queue, but ...
    std::unordered_map<dev_t, std::unique_ptr<io_queue>> _io_queues;
    // ... when dispatched all requests get into this single sink
    internal::io_sink _io_sink;
    unsigned _num_io_groups = 0;

    std::vector<noncopyable_function<future<> ()>> _exit_funcs;
    unsigned _id = 0;
    bool _stopping = false;
    bool _stopped = false;
    bool _finished_running_tasks = false;
    condition_variable _stop_requested;
    std::optional<future<std::unique_ptr<network_stack>>> _network_stack_ready;
    int _return = 0;
    promise<> _start_promise;
    semaphore _cpu_started;
    internal::preemption_monitor _preemption_monitor{};
    uint64_t _global_tasks_processed = 0;
    uint64_t _polls = 0;
    metrics::internal::time_estimated_histogram _stalls_histogram;
    std::unique_ptr<internal::cpu_stall_detector> _cpu_stall_detector;

    timer<>::set_t _timers;
    timer<>::set_t::timer_list_t _expired_timers;
    timer<lowres_clock>::set_t _lowres_timers;
    timer<lowres_clock>::set_t::timer_list_t _expired_lowres_timers;
    timer<manual_clock>::set_t _manual_timers;
    timer<manual_clock>::set_t::timer_list_t _expired_manual_timers;
    io_stats _io_stats;
    uint64_t _fsyncs = 0;
    uint64_t _cxx_exceptions = 0;
    uint64_t _abandoned_failed_futures = 0;
    struct task_queue {
        explicit task_queue(unsigned id, sstring name, sstring shortname, float shares);
        int64_t _vruntime = 0;
        float _shares;
        int64_t _reciprocal_shares_times_2_power_32;
        bool _active = false;
        uint8_t _id;
        sched_clock::time_point _ts; // to help calculating wait/starve-times
        sched_clock::duration _runtime = {};
        sched_clock::duration _waittime = {};
        sched_clock::duration _starvetime = {};
        uint64_t _tasks_processed = 0;
        circular_buffer<task*> _q;
        sstring _name;
        // the shortened version of scheduling_gruop's name, only the first 4
        // chars are used.
        static constexpr size_t shortname_size = 4;
        sstring _shortname;
        int64_t to_vruntime(sched_clock::duration runtime) const;
        void set_shares(float shares) noexcept;
        struct indirect_compare;
        sched_clock::duration _time_spent_on_task_quota_violations = {};
        seastar::metrics::metric_groups _metrics;
        void rename(sstring new_name, sstring new_shortname);
    private:
        void register_stats();
    };

    boost::container::static_vector<std::unique_ptr<task_queue>, max_scheduling_groups()> _task_queues;
    internal::scheduling_group_specific_thread_local_data _scheduling_group_specific_data;
    shared_mutex _scheduling_group_keys_mutex;
    int64_t _last_vruntime = 0;
    task_queue_list _active_task_queues;
    task_queue_list _activating_task_queues;
    task_queue* _at_destroy_tasks;
    task* _current_task = nullptr;
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
    lowres_clock::time_point _lowres_next_timeout = lowres_clock::time_point::max();
    std::optional<pollable_fd> _aio_eventfd;
    const bool _reuseport;
    circular_buffer<double> _loads;
    double _load = 0;
    // Next two fields are required to enforce the monotonicity of total_steal_time()
    // see that method for details.

    // Last measured accumulated steal time, i.e., the simple difference of accumulated
    // awake time and consumed thread CPU time.
    sched_clock::duration _last_true_steal{0};
    // Accumulated steal time forced to be monotinic by rejecting any updates that would
    // decrease it. See total_steal_time() for details.
    sched_clock::duration _last_mono_steal{0};
    sched_clock::duration _total_idle{0};
    sched_clock::duration _total_sleep{0};
    sched_clock::time_point _start_time = now();
    output_stream<char>::batch_flush_list_t _flush_batching;
    std::atomic<bool> _sleeping alignas(seastar::cache_line_size){0};
    pthread_t _thread_id alignas(seastar::cache_line_size) = pthread_self();
    std::atomic<bool> _dying{false};
    gate _background_gate;

    inline auto& get_sg_data(const scheduling_group& sg) {
        return _scheduling_group_specific_data.per_scheduling_group_data[sg._id];
    }

    inline auto& get_sg_data(unsigned sg_id) {
        return _scheduling_group_specific_data.per_scheduling_group_data[sg_id];
    }

private:
    static std::chrono::nanoseconds calculate_poll_time();
    static void block_notifier(int);
    bool flush_pending_aio();
    steady_clock_type::time_point next_pending_aio() const noexcept;
    bool reap_kernel_completions();
    bool flush_tcp_batches();
    void update_lowres_clocks() noexcept;
    bool do_expire_lowres_timers() noexcept;
    bool do_check_lowres_timers() const noexcept;
    void expire_manual_timers() noexcept;
    void start_aio_eventfd_loop();
    void stop_aio_eventfd_loop();

    /**
     * Returns TRUE if all pollers allow blocking.
     *
     * @return FALSE if at least one of the blockers requires a non-blocking
     *         execution.
     */
    bool poll_once();
    bool pure_poll_once();
public:
    /// Register a user-defined signal handler
    [[deprecated("Use seastar::handle_signal(signo, handler, once); instead")]]
    void handle_signal(int signo, noncopyable_function<void ()>&& handler);
    void wakeup();
    /// @private
    bool stopped() const noexcept { return _stopped; }
    /// @private
    uint64_t polls() const noexcept { return _polls; }

private:
    class signals {
    public:
        signals();
        ~signals();

        bool poll_signal();
        bool pure_poll_signal() const;
        void handle_signal(int signo, noncopyable_function<void ()>&& handler);
        void handle_signal_once(int signo, noncopyable_function<void ()>&& handler);
        static void action(int signo, siginfo_t* siginfo, void* ignore);
        static void failed_to_handle(int signo);
    private:
        struct signal_handler {
            signal_handler(int signo, noncopyable_function<void ()>&& handler);
            noncopyable_function<void ()> _handler;
        };
        std::atomic<uint64_t> _pending_signals;
        std::unordered_map<int, signal_handler> _signal_handlers;
    };

    signals _signals;
    std::unique_ptr<thread_pool> _thread_pool;
    friend class thread_pool;
    friend class thread_context;
    friend class internal::cpu_stall_detector;

    friend void handle_signal(int signo, noncopyable_function<void ()>&& handler, bool once);

    uint64_t pending_task_count() const;
    void run_tasks(task_queue& tq);
    bool have_more_tasks() const;
    bool posix_reuseport_detect();
    void run_some_tasks();
    void activate(task_queue& tq);
    void insert_active_task_queue(task_queue* tq);
    task_queue* pop_active_task_queue(sched_clock::time_point now);
    void insert_activating_task_queues();
    void account_runtime(task_queue& tq, sched_clock::duration runtime);
    future<> rename_scheduling_group_specific_data(scheduling_group sg);
    future<> init_scheduling_group(scheduling_group sg, sstring name, sstring shortname, float shares);
    future<> init_new_scheduling_group_key(scheduling_group_key key, scheduling_group_key_config cfg);
    future<> destroy_scheduling_group(scheduling_group sg) noexcept;
    uint64_t tasks_processed() const;
    uint64_t min_vruntime() const;
    void request_preemption();
    void start_handling_signal();
    void reset_preemption_monitor();
    void service_highres_timer() noexcept;

    future<std::tuple<pollable_fd, socket_address>>
    do_accept(pollable_fd_state& listen_fd);
    future<> do_connect(pollable_fd_state& pfd, socket_address& sa);

    future<size_t>
    do_read(pollable_fd_state& fd, void* buffer, size_t size);
    future<size_t>
    do_recvmsg(pollable_fd_state& fd, const std::vector<iovec>& iov);
    future<temporary_buffer<char>>
    do_read_some(pollable_fd_state& fd, internal::buffer_allocator* ba);

    future<size_t>
    do_send(pollable_fd_state& fd, const void* buffer, size_t size);
    future<size_t>
    do_sendmsg(pollable_fd_state& fd, net::packet& p);

    future<temporary_buffer<char>>
    do_recv_some(pollable_fd_state& fd, internal::buffer_allocator* ba);

    void configure(const reactor_options& opts);
    int do_run();
    // Waits for all background tasks on all shards
    static future<> drain();

public:
    explicit reactor(std::shared_ptr<smp> smp, alien::instance& alien, unsigned id, reactor_backend_selector rbs, reactor_config cfg);
    reactor(const reactor&) = delete;
    ~reactor();
    void operator=(const reactor&) = delete;

    static sched_clock::time_point now() noexcept {
        return sched_clock::now();
    }
    sched_clock::duration uptime() {
        return now() - _start_time;
    }

    io_queue& get_io_queue(dev_t devid = 0) {
        auto queue = _io_queues.find(devid);
        if (queue == _io_queues.end()) {
            return *_io_queues.at(0);
        } else {
            return *(queue->second);
        }
    }

    /// @private
    future<> update_bandwidth_for_queues(internal::priority_class pc, uint64_t bandwidth);
    /// @private
    void rename_queues(internal::priority_class pc, sstring new_name);
    /// @private
    void update_shares_for_queues(internal::priority_class pc, uint32_t shares);

    server_socket listen(socket_address sa, listen_options opts = {});

    future<connected_socket> connect(socket_address sa);
    future<connected_socket> connect(socket_address, socket_address, transport proto = transport::TCP);

    pollable_fd posix_listen(socket_address sa, listen_options opts = {});

    bool posix_reuseport_available() const { return _reuseport; }

    pollable_fd make_pollable_fd(socket_address sa, int proto);

    future<> posix_connect(pollable_fd pfd, socket_address sa, socket_address local);

    future<> send_all(pollable_fd_state& fd, const void* buffer, size_t size);

    future<file> open_file_dma(std::string_view name, open_flags flags, file_open_options options = {}) noexcept;
    future<file> open_directory(std::string_view name) noexcept;
    future<> make_directory(std::string_view name, file_permissions permissions = file_permissions::default_dir_permissions) noexcept;
    future<> touch_directory(std::string_view name, file_permissions permissions = file_permissions::default_dir_permissions) noexcept;
    future<std::optional<directory_entry_type>>  file_type(std::string_view name, follow_symlink = follow_symlink::yes) noexcept;
    future<stat_data> file_stat(std::string_view pathname, follow_symlink) noexcept;
    future<> chown(std::string_view filepath, uid_t owner, gid_t group);
    future<std::optional<struct group_details>> getgrnam(std::string_view name);
    future<uint64_t> file_size(std::string_view pathname) noexcept;
    future<bool> file_accessible(std::string_view pathname, access_flags flags) noexcept;
    future<bool> file_exists(std::string_view pathname) noexcept {
        return file_accessible(pathname, access_flags::exists);
    }
    future<fs_type> file_system_at(std::string_view pathname) noexcept;
    future<std::filesystem::space_info> file_system_space(std::string_view pathname) noexcept;
    future<struct statvfs> statvfs(std::string_view pathname) noexcept;
    future<> remove_file(std::string_view pathname) noexcept;
    future<> rename_file(std::string_view old_pathname, std::string_view new_pathname) noexcept;
    future<> link_file(std::string_view oldpath, std::string_view newpath) noexcept;
    future<> chmod(std::string_view name, file_permissions permissions) noexcept;

    future<size_t> read_directory(int fd, char* buffer, size_t buffer_size);

    future<int> inotify_add_watch(int fd, std::string_view path, uint32_t flags);

    future<std::tuple<file_desc, file_desc>> make_pipe();
    future<std::tuple<pid_t, file_desc, file_desc, file_desc>>
    spawn(std::string_view pathname,
          std::vector<sstring> argv,
          std::vector<sstring> env = {});
    future<int> waitpid(pid_t pid);
    void kill(pid_t pid, int sig);

    int run() noexcept;
    void exit(int ret);
    future<> when_started() { return _start_promise.get_future(); }
    // The function waits for timeout period for reactor stop notification
    // which happens on termination signals or call for exit().
    template <typename Rep, typename Period>
    future<> wait_for_stop(std::chrono::duration<Rep, Period> timeout) {
        return _stop_requested.wait(timeout, [this] { return _stopping; });
    }

    void at_exit(noncopyable_function<future<> ()> func);

    template <typename Func>
    void at_destroy(Func&& func) {
        _at_destroy_tasks->_q.push_back(make_task(default_scheduling_group(), std::forward<Func>(func)));
    }

    task* current_task() const { return _current_task; }
    // If a task wants to resume a different task instead of returning control to the reactor,
    // it should set _current_task to the resumed task.
    // In particular, this is mandatory if the task is going to die before control is returned
    // to the reactor -- otherwise _current_task will be left dangling.
    void set_current_task(task* t) { _current_task = t; }

    void add_task(task* t) noexcept;
    void add_urgent_task(task* t) noexcept;

    void run_in_background(future<> f);

    template <typename Func>
    void run_in_background(Func&& func) {
        run_in_background(futurize_invoke(std::forward<Func>(func)));
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

    void add_high_priority_task(task*) noexcept;

    network_stack& net() { return *_network_stack; }

    [[deprecated("Use this_shard_id")]]
    shard_id cpu_id() const;

    void try_sleep();

    steady_clock_type::duration total_idle_time();
    steady_clock_type::duration total_busy_time();
    steady_clock_type::duration total_awake_time() const;
    std::chrono::nanoseconds total_cpu_time() const;
    std::chrono::nanoseconds total_steal_time();

    const io_stats& get_io_stats() const { return _io_stats; }
    /// Returns statistics related to scheduling. The statistics are
    /// local to this shard.
    ///
    /// See \ref sched_stats for a description of individual statistics.
    /// \return An object containing a snapshot of the statistics at this point in time.
    sched_stats get_sched_stats() const;
    uint64_t abandoned_failed_futures() const { return _abandoned_failed_futures; }
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
    future<> send_all_part(pollable_fd_state& fd, const void* buffer, size_t size, size_t completed);

    future<> fdatasync(int fd) noexcept;

    void add_timer(timer<steady_clock_type>*) noexcept;
    bool queue_timer(timer<steady_clock_type>*) noexcept;
    void del_timer(timer<steady_clock_type>*) noexcept;
    void add_timer(timer<lowres_clock>*) noexcept;
    bool queue_timer(timer<lowres_clock>*) noexcept;
    void del_timer(timer<lowres_clock>*) noexcept;
    void add_timer(timer<manual_clock>*) noexcept;
    bool queue_timer(timer<manual_clock>*) noexcept;
    void del_timer(timer<manual_clock>*) noexcept;

    future<> run_exit_tasks();
    void stop();
    friend class pollable_fd;
    friend class pollable_fd_state;
    friend class posix_file_impl;
    friend class blockdev_file_impl;
    friend class timer<>;
    friend class timer<lowres_clock>;
    friend class timer<manual_clock>;
    friend class smp;
    friend class internal::poller;
    friend class scheduling_group;
    friend void internal::add_to_flush_poller(output_stream<char>& os) noexcept;
    friend void seastar::internal::increase_thrown_exceptions_counter() noexcept;
    friend void report_failed_future(const std::exception_ptr& eptr) noexcept;
    metrics::metric_groups _metric_groups;
    friend future<scheduling_group> create_scheduling_group(sstring name, sstring shortname, float shares) noexcept;
    friend future<> seastar::destroy_scheduling_group(scheduling_group) noexcept;
    friend future<> seastar::rename_scheduling_group(scheduling_group sg, sstring new_name, sstring new_shortname) noexcept;
    friend future<scheduling_group_key> scheduling_group_key_create(scheduling_group_key_config cfg) noexcept;
    friend seastar::internal::log_buf::inserter_iterator do_dump_task_queue(seastar::internal::log_buf::inserter_iterator it, const task_queue& tq);

    future<struct statfs> fstatfs(int fd) noexcept;
    friend future<shared_ptr<file_impl>> make_file_impl(int fd, file_open_options options, int flags, struct stat st) noexcept;
public:
    future<> readable(pollable_fd_state& fd);
    future<> writeable(pollable_fd_state& fd);
    future<> readable_or_writeable(pollable_fd_state& fd);
    future<> poll_rdhup(pollable_fd_state& fd);
    void enable_timer(steady_clock_type::time_point when) noexcept;
    /// Sets the "Strict DMA" flag.
    ///
    /// When true (default), file I/O operations must use DMA.  This is
    /// the most performant option, but does not work on some file systems
    /// such as tmpfs or aufs (used in some Docker setups).
    ///
    /// When false, file I/O operations can fall back to buffered I/O if
    /// DMA is not available.  This can result in dramatic reducation in
    /// performance and an increase in memory consumption.
    void set_strict_dma(bool value);
    void set_bypass_fsync(bool value);
    void update_blocked_reactor_notify_ms(std::chrono::milliseconds ms);
    std::chrono::milliseconds get_blocked_reactor_notify_ms() const;

    class test {
    public:
        static void with_allow_abandoned_failed_futures(unsigned count, noncopyable_function<void ()> func);

        /// Sets the stall reporting function which is called when a stall
        /// is detected (and not suppressed). Setting the function also
        /// resets the supression state.
        static void set_stall_detector_report_function(std::function<void ()> report);
        static std::function<void ()> get_stall_detector_report_function();
    };
};

extern __thread reactor* local_engine;
extern __thread size_t task_quota;

SEASTAR_MODULE_EXPORT
inline reactor& engine() {
    return *local_engine;
}

SEASTAR_MODULE_EXPORT
inline bool engine_is_ready() {
    return local_engine != nullptr;
}

inline int hrtimer_signal() {
    // We don't want to use SIGALRM, because the boost unit test library
    // also plays with it.
    return SIGRTMIN;
}


extern logger seastar_logger;

}
