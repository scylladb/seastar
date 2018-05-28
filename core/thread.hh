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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once

#include "thread_impl.hh"
#include "future.hh"
#include "do_with.hh"
#include "future-util.hh"
#include "timer.hh"
#include "reactor.hh"
#include "scheduling.hh"
#include <memory>
#include <setjmp.h>
#include <type_traits>
#include <chrono>
#include <experimental/optional>
#include <ucontext.h>
#include <boost/intrusive/list.hpp>

/// \defgroup thread-module Seastar threads
///
/// Seastar threads provide an execution environment where blocking
/// is tolerated; you can issue I/O, and wait for it in the same function,
/// rather then establishing a callback to be called with \ref future<>::then().
///
/// Seastar threads are not the same as operating system threads:
///   - seastar threads are cooperative; they are never preempted except
///     at blocking points (see below)
///   - seastar threads always run on the same core they were launched on
///
/// Like other seastar code, seastar threads may not issue blocking system calls.
///
/// A seastar thread blocking point is any function that returns a \ref future<>.
/// you block by calling \ref future<>::get(); this waits for the future to become
/// available, and in the meanwhile, other seastar threads and seastar non-threaded
/// code may execute.
///
/// Example:
/// \code
///    seastar::thread th([] {
///       sleep(5s).get();  // blocking point
///    });
/// \endcode
///
/// An easy way to launch a thread and carry out some computation, and return a
/// result from this execution is by using the \ref seastar::async() function.
/// The result is returned as a future, so that non-threaded code can wait for
/// the thread to terminate and yield a result.

/// Seastar API namespace
namespace seastar {

namespace stdx = std::experimental;

/// \addtogroup thread-module
/// @{

class thread;
class thread_attributes;
class thread_scheduling_group;

/// Class that holds attributes controling the behavior of a thread.
class thread_attributes {
public:
    thread_scheduling_group* scheduling_group = nullptr;  // FIXME: remove
    stdx::optional<seastar::scheduling_group> sched_group;
};


/// \cond internal
extern thread_local jmp_buf_link g_unthreaded_context;

// Internal class holding thread state.  We can't hold this in
// \c thread itself because \c thread is movable, and we want pointers
// to this state to be captured.
class thread_context {
    struct stack_deleter {
        void operator()(char *ptr) const noexcept;
    };
    using stack_holder = std::unique_ptr<char[], stack_deleter>;
    static constexpr size_t base_stack_size = 128*1024;

    thread_attributes _attr;
#ifdef SEASTAR_THREAD_STACK_GUARDS
    const size_t _stack_size;
#else
    static constexpr size_t _stack_size = base_stack_size;
#endif
    stack_holder _stack{make_stack()};
    std::function<void ()> _func;
    jmp_buf_link _context;
    scheduling_group _scheduling_group;
    promise<> _done;
    bool _joined = false;
    timer<> _sched_timer{[this] { reschedule(); }};
    stdx::optional<promise<>> _sched_promise;

    boost::intrusive::list_member_hook<> _preempted_link;
    using preempted_thread_list = boost::intrusive::list<thread_context,
        boost::intrusive::member_hook<thread_context, boost::intrusive::list_member_hook<>,
        &thread_context::_preempted_link>,
        boost::intrusive::constant_time_size<false>>;

    boost::intrusive::list_member_hook<> _all_link;
    using all_thread_list = boost::intrusive::list<thread_context,
        boost::intrusive::member_hook<thread_context, boost::intrusive::list_member_hook<>,
        &thread_context::_all_link>,
        boost::intrusive::constant_time_size<false>>;

    static thread_local preempted_thread_list _preempted_threads;
    static thread_local all_thread_list _all_threads;
private:
    static void s_main(int lo, int hi); // all parameters MUST be 'int' for makecontext
    void setup();
    void main();
    stack_holder make_stack();
public:
    thread_context(thread_attributes attr, std::function<void ()> func);
    ~thread_context();
    void switch_in();
    void switch_out();
    bool should_yield() const;
    void reschedule();
    void yield();
    friend class thread;
    friend void thread_impl::switch_in(thread_context*);
    friend void thread_impl::switch_out(thread_context*);
    friend scheduling_group thread_impl::sched_group(const thread_context*);
};

/// \endcond


/// \brief thread - stateful thread of execution
///
/// Threads allow using seastar APIs in a blocking manner,
/// by calling future::get() on a non-ready future.  When
/// this happens, the thread is put to sleep until the future
/// becomes ready.
class thread {
    std::unique_ptr<thread_context> _context;
    static thread_local thread* _current;
public:
    /// \brief Constructs a \c thread object that does not represent a thread
    /// of execution.
    thread() = default;
    /// \brief Constructs a \c thread object that represents a thread of execution
    ///
    /// \param func Callable object to execute in thread.  The callable is
    ///             called immediately.
    template <typename Func>
    thread(Func func);
    /// \brief Constructs a \c thread object that represents a thread of execution
    ///
    /// \param attr Attributes describing the new thread.
    /// \param func Callable object to execute in thread.  The callable is
    ///             called immediately.
    template <typename Func>
    thread(thread_attributes attr, Func func);
    /// \brief Moves a thread object.
    thread(thread&& x) noexcept = default;
    /// \brief Move-assigns a thread object.
    thread& operator=(thread&& x) noexcept = default;
    /// \brief Destroys a \c thread object.
    ///
    /// The thread must not represent a running thread of execution (see join()).
    ~thread() { assert(!_context || _context->_joined); }
    /// \brief Waits for thread execution to terminate.
    ///
    /// Waits for thread execution to terminate, and marks the thread object as not
    /// representing a running thread of execution.
    future<> join();
    /// \brief Voluntarily defer execution of current thread.
    ///
    /// Gives other threads/fibers a chance to run on current CPU.
    /// The current thread will resume execution promptly.
    static void yield();
    /// \brief Checks whether this thread ought to call yield() now.
    ///
    /// Useful where we cannot call yield() immediately because we
    /// Need to take some cleanup action first.
    static bool should_yield();

    static bool running_in_thread() {
        return thread_impl::get() != nullptr;
    }
private:
    friend class reactor;
    // To be used by seastar reactor only.
    static bool try_run_one_yielded_thread();
};

/// An instance of this class can be used to assign a thread to a particular scheduling group.
/// Threads can share the same scheduling group if they hold a pointer to the same instance
/// of this class.
///
/// All threads that belongs to a scheduling group will have a time granularity defined by \c period,
/// and can specify a fraction \c usage of that period that indicates the maximum amount of time they
/// expect to run. \c usage, is expected to be a number between 0 and 1 for this to have any effect.
/// Numbers greater than 1 are allowed for simplicity, but they just have the same meaning of 1, alas,
/// "the whole period".
///
/// Note that this is not a preemptive runtime, and a thread will not exit the CPU unless it is scheduled out.
/// In that case, \c usage will not be enforced and the thread will simply run until it loses the CPU.
/// This can happen when a thread waits on a future that is not ready, or when it voluntarily call yield.
///
/// Unlike what happens for a thread that is not part of a scheduling group - which puts itself at the back
/// of the runqueue everytime it yields, a thread that is part of a scheduling group will only yield if
/// it has exhausted its \c usage at the call to yield. Therefore, threads in a schedule group can and
/// should yield often.
///
/// After those events, if the thread has already run for more than its fraction, it will be scheduled to
/// run again only after \c period completes, unless there are no other tasks to run (the system is
/// idle)
class thread_scheduling_group {
    std::chrono::nanoseconds _period;
    std::chrono::nanoseconds _quota;
    std::chrono::time_point<thread_clock> _this_period_ends = {};
    std::chrono::time_point<thread_clock> _this_run_start = {};
    std::chrono::nanoseconds _this_period_remain = {};
public:
    /// \brief Constructs a \c thread_scheduling_group object
    ///
    /// \param period a duration representing the period
    /// \param usage which fraction of the \c period to assign for the scheduling group. Expected between 0 and 1.
    thread_scheduling_group(std::chrono::nanoseconds period, float usage);
    /// \brief changes the current maximum usage per period
    ///
    /// \param new_usage The new fraction of the \c period (Expected between 0 and 1) during which to run
    void update_usage(float new_usage) {
        _quota = std::chrono::duration_cast<std::chrono::nanoseconds>(new_usage * _period);
    }
private:
    void account_start();
    void account_stop();
    stdx::optional<thread_clock::time_point> next_scheduling_point() const;
    friend class thread_context;
};

template <typename Func>
inline
thread::thread(thread_attributes attr, Func func)
        : _context(std::make_unique<thread_context>(std::move(attr), func)) {
}

template <typename Func>
inline
thread::thread(Func func)
        : thread(thread_attributes(), std::move(func)) {
}

inline
future<>
thread::join() {
    _context->_joined = true;
    return _context->_done.get_future();
}

/// Executes a callable in a seastar thread.
///
/// Runs a block of code in a threaded context,
/// which allows it to block (using \ref future::get()).  The
/// result of the callable is returned as a future.
///
/// \param attr a \ref thread_attributes instance
/// \param func a callable to be executed in a thread
/// \param args a parameter pack to be forwarded to \c func.
/// \return whatever \c func returns, as a future.
///
/// Example:
/// \code
///    future<int> compute_sum(int a, int b) {
///        thread_attributes attr = {};
///        attr.scheduling_group = some_scheduling_group_ptr;
///        return seastar::async(attr, [a, b] {
///            // some blocking code:
///            sleep(1s).get();
///            return a + b;
///        });
///    }
/// \endcode
template <typename Func, typename... Args>
inline
futurize_t<std::result_of_t<std::decay_t<Func>(std::decay_t<Args>...)>>
async(thread_attributes attr, Func&& func, Args&&... args) {
    using return_type = std::result_of_t<std::decay_t<Func>(std::decay_t<Args>...)>;
    struct work {
        thread_attributes attr;
        Func func;
        std::tuple<Args...> args;
        promise<return_type> pr;
        thread th;
    };
    return do_with(work{std::move(attr), std::forward<Func>(func), std::forward_as_tuple(std::forward<Args>(args)...)}, [] (work& w) mutable {
        auto ret = w.pr.get_future();
        w.th = thread(std::move(w.attr), [&w] {
            futurize<return_type>::apply(std::move(w.func), std::move(w.args)).forward_to(std::move(w.pr));
        });
        return w.th.join().then([ret = std::move(ret)] () mutable {
            return std::move(ret);
        });
    });
}

/// Executes a callable in a seastar thread.
///
/// Runs a block of code in a threaded context,
/// which allows it to block (using \ref future::get()).  The
/// result of the callable is returned as a future.
///
/// \param func a callable to be executed in a thread
/// \param args a parameter pack to be forwarded to \c func.
/// \return whatever \c func returns, as a future.
template <typename Func, typename... Args>
inline
futurize_t<std::result_of_t<std::decay_t<Func>(std::decay_t<Args>...)>>
async(Func&& func, Args&&... args) {
    return async(thread_attributes{}, std::forward<Func>(func), std::forward<Args>(args)...);
}
/// @}

}
