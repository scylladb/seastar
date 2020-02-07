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
#include <seastar/core/semaphore.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/reactor_config.hh>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/range/irange.hpp>
#include <boost/program_options.hpp>
#include <deque>
#include <thread>

/// \file

namespace seastar {

using shard_id = unsigned;

class smp_service_group;
class reactor_backend_selector;

namespace internal {

unsigned smp_service_group_id(smp_service_group ssg);

inline shard_id* this_shard_id_ptr() {
    static thread_local shard_id g_this_shard_id;
    return &g_this_shard_id;
}

}

/// Returns shard_id of the of the current shard.
inline shard_id this_shard_id() {
    return *internal::this_shard_id_ptr();
}

/// Configuration for smp_service_group objects.
///
/// \see create_smp_service_group()
struct smp_service_group_config {
    /// The maximum number of non-local requests that execute on a shard concurrently
    ///
    /// Will be adjusted upwards to allow at least one request per non-local shard.
    unsigned max_nonlocal_requests = 0;
};

/// A resource controller for cross-shard calls.
///
/// An smp_service_group allows you to limit the concurrency of
/// smp::submit_to() and similar calls. While it's easy to limit
/// the caller's concurrency (for example, by using a semaphore),
/// the concurrency at the remote end can be multiplied by a factor
/// of smp::count-1, which can be large.
///
/// The class is called a service _group_ because it can be used
/// to group similar calls that share resource usage characteristics,
/// need not be isolated from each other, but do need to be isolated
/// from other groups. Calls in a group should not nest; doing so
/// can result in ABA deadlocks.
///
/// Nested submit_to() calls must form a directed acyclic graph
/// when considering their smp_service_groups as nodes. For example,
/// if a call using ssg1 then invokes another call using ssg2, the
/// internal call may not call again via either ssg1 or ssg2, or it
/// may form a cycle (and risking an ABBA deadlock). Create a
/// new smp_service_group_instead.
class smp_service_group {
    unsigned _id;
private:
    explicit smp_service_group(unsigned id) : _id(id) {}

    friend unsigned internal::smp_service_group_id(smp_service_group ssg);
    friend smp_service_group default_smp_service_group();
    friend future<smp_service_group> create_smp_service_group(smp_service_group_config ssgc);
};

inline
unsigned
internal::smp_service_group_id(smp_service_group ssg) {
    return ssg._id;
}

/// Returns the default smp_service_group. This smp_service_group
/// does not impose any limits on concurrency in the target shard.
/// This makes is deadlock-safe, but can consume unbounded resources,
/// and should therefore only be used when initiator concurrency is
/// very low (e.g. administrative tasks).
smp_service_group default_smp_service_group();

/// Creates an smp_service_group with the specified configuration.
///
/// The smp_service_group is global, and after this call completes,
/// the returned value can be used on any shard.
future<smp_service_group> create_smp_service_group(smp_service_group_config ssgc);

/// Destroy an smp_service_group.
///
/// Frees all resources used by an smp_service_group. It must not
/// be used again once this function is called.
future<> destroy_smp_service_group(smp_service_group ssg);

inline
smp_service_group default_smp_service_group() {
    return smp_service_group(0);
}

using smp_timeout_clock = lowres_clock;
using smp_service_group_semaphore = basic_semaphore<named_semaphore_exception_factory, smp_timeout_clock>;
using smp_service_group_semaphore_units = semaphore_units<named_semaphore_exception_factory, smp_timeout_clock>;

static constexpr smp_timeout_clock::time_point smp_no_timeout = smp_timeout_clock::time_point::max();

/// Options controlling the behaviour of \ref smp::submit_to().
struct smp_submit_to_options {
    /// Controls resource allocation.
    smp_service_group service_group = default_smp_service_group();
    /// The timeout is relevant only to the time the call spends waiting to be
    /// processed by the remote shard, and *not* to the time it takes to be
    /// executed there.
    smp_timeout_clock::time_point timeout = smp_no_timeout;

    smp_submit_to_options(smp_service_group service_group = default_smp_service_group(), smp_timeout_clock::time_point timeout = smp_no_timeout)
        : service_group(service_group)
        , timeout(timeout) {
    }
};

void init_default_smp_service_group(shard_id cpu);

smp_service_group_semaphore& get_smp_service_groups_semaphore(unsigned ssg_id, shard_id t);

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
        ~lf_queue();
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
    // between them, so hw prefetcher will not accidentally prefetch
    // cache line used by another cpu.
    metrics::metric_groups _metrics;
    struct alignas(seastar::cache_line_size) {
        size_t _received = 0;
        size_t _last_rcv_batch = 0;
    };
    struct work_item : public task {
        explicit work_item(smp_service_group ssg) : task(current_scheduling_group()), ssg(ssg) {}
        smp_service_group ssg;
        virtual ~work_item() {}
        virtual void fail_with(std::exception_ptr) = 0;
        void process();
        virtual void complete() = 0;
    };
    template <typename Func>
    struct async_work_item : work_item {
        smp_message_queue& _queue;
        Func _func;
        using futurator = futurize<std::result_of_t<Func()>>;
        using future_type = typename futurator::type;
        using value_type = typename future_type::value_type;
        compat::optional<value_type> _result;
        std::exception_ptr _ex; // if !_result
        typename futurator::promise_type _promise; // used on local side
        async_work_item(smp_message_queue& queue, smp_service_group ssg, Func&& func) : work_item(ssg), _queue(queue), _func(std::move(func)) {}
        virtual void fail_with(std::exception_ptr ex) override {
            _promise.set_exception(std::move(ex));
        }
        virtual task* waiting_task() noexcept override {
            // FIXME: waiting_tasking across shards is not implemented. Unsynchronized task access is unsafe.
            return nullptr;
        }
        virtual void run_and_dispose() noexcept override {
            // _queue.respond() below forwards the continuation chain back to the
            // calling shard.
            (void)futurator::invoke(this->_func).then_wrapped([this] (auto f) {
                if (f.failed()) {
                    _ex = f.get_exception();
                } else {
                    _result = f.get();
                }
                _queue.respond(this);
            });
            // We don't delete the task here as the creator of the work item will
            // delete it on the origin shard.
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
    ~smp_message_queue();
    template <typename Func>
    futurize_t<std::result_of_t<Func()>> submit(shard_id t, smp_submit_to_options options, Func&& func) noexcept {
        memory::disable_failure_guard dfg;
        auto wi = std::make_unique<async_work_item<Func>>(*this, options.service_group, std::forward<Func>(func));
        auto fut = wi->get_future();
        submit_item(t, options.timeout, std::move(wi));
        return fut;
    }
    void start(unsigned cpuid);
    template<size_t PrefetchCnt, typename Func>
    size_t process_queue(lf_queue& q, Func process);
    size_t process_incoming();
    size_t process_completions(shard_id t);
    void stop();
private:
    void work();
    void submit_item(shard_id t, smp_timeout_clock::time_point timeout, std::unique_ptr<work_item> wi);
    void respond(work_item* wi);
    void move_pending();
    void flush_request_batch();
    void flush_response_batch();
    bool has_unflushed_responses() const;
    bool pure_poll_rx() const;
    bool pure_poll_tx() const;

    friend class smp;
};

class smp {
    static std::vector<posix_thread> _threads;
    static std::vector<std::function<void ()>> _thread_loops; // for dpdk
    static compat::optional<boost::barrier> _all_event_loops_done;
    static std::vector<reactor*> _reactors;
    struct qs_deleter {
      void operator()(smp_message_queue** qs) const;
    };
    static std::unique_ptr<smp_message_queue*[], qs_deleter> _qs;
    static std::thread::id _tmain;
    static bool _using_dpdk;

    template <typename Func>
    using returns_future = is_future<std::result_of_t<Func()>>;
    template <typename Func>
    using returns_void = std::is_same<std::result_of_t<Func()>, void>;
public:
    static boost::program_options::options_description get_options_description();
    static void register_network_stacks();
    static void configure(boost::program_options::variables_map vm, reactor_config cfg = {});
    static void cleanup();
    static void cleanup_cpu();
    static void arrive_at_event_loop_end();
    static void join_all();
    static bool main_thread() { return std::this_thread::get_id() == _tmain; }

    /// Runs a function on a remote core.
    ///
    /// \param t designates the core to run the function on (may be a remote
    ///          core or the local core).
    /// \param options an \ref smp_submit_to_options that contains options for this call.
    /// \param func a callable to run on core \c t.
    ///          If \c func is a temporary object, its lifetime will be
    ///          extended by moving. This movement and the eventual
    ///          destruction of func are both done in the _calling_ core.
    ///          If \c func is a reference, the caller must guarantee that
    ///          it will survive the call.
    /// \return whatever \c func returns, as a future<> (if \c func does not return a future,
    ///         submit_to() will wrap it in a future<>).
    template <typename Func>
    static futurize_t<std::result_of_t<Func()>> submit_to(unsigned t, smp_submit_to_options options, Func&& func) {
        using ret_type = std::result_of_t<Func()>;
        if (t == this_shard_id()) {
            try {
                if (!is_future<ret_type>::value) {
                    // Non-deferring function, so don't worry about func lifetime
                    return futurize<ret_type>::invoke(std::forward<Func>(func));
                } else if (std::is_lvalue_reference<Func>::value) {
                    // func is an lvalue, so caller worries about its lifetime
                    return futurize<ret_type>::invoke(func);
                } else {
                    // Deferring call on rvalue function, make sure to preserve it across call
                    auto w = std::make_unique<std::decay_t<Func>>(std::move(func));
                    auto ret = futurize<ret_type>::invoke(*w);
                    return ret.finally([w = std::move(w)] {});
                }
            } catch (...) {
                // Consistently return a failed future rather than throwing, to simplify callers
                return futurize<std::result_of_t<Func()>>::make_exception_future(std::current_exception());
            }
        } else {
            return _qs[t][this_shard_id()].submit(t, options, std::forward<Func>(func));
        }
    }
    /// Runs a function on a remote core.
    ///
    /// Uses default_smp_service_group() to control resource allocation.
    ///
    /// \param t designates the core to run the function on (may be a remote
    ///          core or the local core).
    /// \param func a callable to run on core \c t.
    ///          If \c func is a temporary object, its lifetime will be
    ///          extended by moving. This movement and the eventual
    ///          destruction of func are both done in the _calling_ core.
    ///          If \c func is a reference, the caller must guarantee that
    ///          it will survive the call.
    /// \return whatever \c func returns, as a future<> (if \c func does not return a future,
    ///         submit_to() will wrap it in a future<>).
    template <typename Func>
    static futurize_t<std::result_of_t<Func()>> submit_to(unsigned t, Func&& func) {
        return submit_to(t, default_smp_service_group(), std::forward<Func>(func));
    }
    static bool poll_queues();
    static bool pure_poll_queues();
    static boost::integer_range<unsigned> all_cpus() {
        return boost::irange(0u, count);
    }
    /// Invokes func on all shards.
    ///
    /// \param options the options to forward to the \ref smp::submit_to()
    ///         called behind the scenes.
    /// \param func the function to be invoked on each shard. May return void or
    ///         future<>. Each async invocation will work with a separate copy
    ///         of \c func.
    /// \returns a future that resolves when all async invocations finish.
    template<typename Func>
    static future<> invoke_on_all(smp_submit_to_options options, Func&& func) {
        static_assert(std::is_same<future<>, typename futurize<std::result_of_t<Func()>>::type>::value, "bad Func signature");
        return parallel_for_each(all_cpus(), [options, &func] (unsigned id) {
            return smp::submit_to(id, options, Func(func));
        });
    }
    /// Invokes func on all shards.
    ///
    /// \param func the function to be invoked on each shard. May return void or
    ///         future<>. Each async invocation will work with a separate copy
    ///         of \c func.
    /// \returns a future that resolves when all async invocations finish.
    ///
    /// Passes the default \ref smp_submit_to_options to the
    /// \ref smp::submit_to() called behind the scenes.
    template<typename Func>
    static future<> invoke_on_all(Func&& func) {
        return invoke_on_all(smp_submit_to_options{}, std::forward<Func>(func));
    }
    /// Invokes func on all other shards.
    ///
    /// \param cpu_id the cpu on which **not** to run the function.
    /// \param options the options to forward to the \ref smp::submit_to()
    ///         called behind the scenes.
    /// \param func the function to be invoked on each shard. May return void or
    ///         future<>. Each async invocation will work with a separate copy
    ///         of \c func.
    /// \returns a future that resolves when all async invocations finish.
    template<typename Func>
    static future<> invoke_on_others(unsigned cpu_id, smp_submit_to_options options, Func func) {
        static_assert(std::is_same<future<>, typename futurize<std::result_of_t<Func()>>::type>::value, "bad Func signature");
        return parallel_for_each(all_cpus(), [cpu_id, options, func = std::move(func)] (unsigned id) {
            return id != cpu_id ? smp::submit_to(id, options, func) : make_ready_future<>();
        });
    }
    /// Invokes func on all other shards.
    ///
    /// \param cpu_id the cpu on which **not** to run the function.
    /// \param func the function to be invoked on each shard. May return void or
    ///         future<>. Each async invocation will work with a separate copy
    ///         of \c func.
    /// \returns a future that resolves when all async invocations finish.
    ///
    /// Passes the default \ref smp_submit_to_options to the
    /// \ref smp::submit_to() called behind the scenes.
    template<typename Func>
    static future<> invoke_on_others(unsigned cpu_id, Func func) {
        return invoke_on_others(cpu_id, smp_submit_to_options{}, std::move(func));
    }
private:
    static void start_all_queues();
    static void pin(unsigned cpu_id);
    static void allocate_reactor(unsigned id, reactor_backend_selector rbs, reactor_config cfg);
    static void create_thread(std::function<void ()> thread_loop);
public:
    static unsigned count;
};

}
