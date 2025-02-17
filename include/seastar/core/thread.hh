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

#ifndef SEASTAR_MODULE
#include <seastar/core/thread_impl.hh>
#include <seastar/core/future.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/scheduling.hh>
#include <memory>
#include <type_traits>
#include <seastar/util/assert.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/modules.hh>
#include <ucontext.h>
#include <boost/intrusive/list.hpp>
#endif

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
/// A seastar thread blocking point is any function that returns a \ref future.
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

/// \addtogroup thread-module
/// @{
SEASTAR_MODULE_EXPORT_BEGIN
class thread;
class thread_attributes;

/// Class that holds attributes controling the behavior of a thread.
class thread_attributes {
public:
    std::optional<seastar::scheduling_group> sched_group;
    // For stack_size 0, a default value will be used (128KiB when writing this comment)
    size_t stack_size = 0;
};
SEASTAR_MODULE_EXPORT_END

/// \cond internal
extern thread_local jmp_buf_link g_unthreaded_context;

// Internal class holding thread state.  We can't hold this in
// \c thread itself because \c thread is movable, and we want pointers
// to this state to be captured.
class thread_context final : private task {
    struct stack_deleter {
        void operator()(char *ptr) const noexcept;
        int valgrind_id;
        stack_deleter(int valgrind_id);
    };
    using stack_holder = std::unique_ptr<char[], stack_deleter>;

    stack_holder _stack;
    noncopyable_function<void ()> _func;
    jmp_buf_link _context;
    promise<> _done;
    bool _joined = false;

    boost::intrusive::list_member_hook<> _all_link;
    using all_thread_list = boost::intrusive::list<thread_context,
        boost::intrusive::member_hook<thread_context, boost::intrusive::list_member_hook<>,
        &thread_context::_all_link>,
        boost::intrusive::constant_time_size<false>>;

    static thread_local all_thread_list _all_threads;
private:
    static void s_main(int lo, int hi); // all parameters MUST be 'int' for makecontext
    void setup(size_t stack_size);
    void main();
    stack_holder make_stack(size_t stack_size);
    virtual void run_and_dispose() noexcept override; // from task class
public:
    thread_context(thread_attributes attr, noncopyable_function<void ()> func);
    ~thread_context();
    void switch_in();
    void switch_out();
    bool should_yield() const;
    void reschedule();
    void yield();
    task* waiting_task() noexcept override { return _done.waiting_task(); }
    friend class thread;
    friend void thread_impl::switch_in(thread_context*);
    friend void thread_impl::switch_out(thread_context*);
    friend scheduling_group thread_impl::sched_group(const thread_context*);
};

/// \endcond

SEASTAR_MODULE_EXPORT
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
    ~thread() { SEASTAR_ASSERT(!_context || _context->_joined); }
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
    static bool should_yield() {
        return need_preempt();
    }

    /// \brief Yield if this thread ought to call yield() now.
    ///
    /// Useful where a code does long running computation and does
    /// not want to hog cpu for more then its share
    static void maybe_yield() {
        if (should_yield()) [[unlikely]] {
            yield();
        }
    }

    static bool running_in_thread() {
        return thread_impl::get() != nullptr;
    }
};

template <typename Func>
inline
thread::thread(thread_attributes attr, Func func)
        : _context(std::make_unique<thread_context>(std::move(attr), std::move(func))) {
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

SEASTAR_MODULE_EXPORT_BEGIN
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
///        attr.sched_group = some_scheduling_group_ptr;
///        return seastar::async(attr, [a, b] {
///            // some blocking code:
///            sleep(1s).get();
///            return a + b;
///        });
///    }
/// \endcode
template <typename Func, typename... Args>
inline
futurize_t<std::invoke_result_t<Func, Args...>>
async(thread_attributes attr, Func&& func, Args&&... args) noexcept {
    using return_type = std::invoke_result_t<Func, Args...>;
    struct work {
        thread_attributes attr;
        Func func;
        std::tuple<Args...> args;
        promise<return_type> pr{};
        thread th{};
    };

    try {
        auto wp = std::make_unique<work>(work{std::move(attr), std::forward<Func>(func), std::forward_as_tuple(std::forward<Args>(args)...)});
        auto& w = *wp;
        auto ret = w.pr.get_future();
        w.th = thread(std::move(w.attr), [&w] {
            futurize<return_type>::apply(std::move(w.func), std::move(w.args)).forward_to(std::move(w.pr));
        });
        return w.th.join().then([ret = std::move(ret)] () mutable {
            return std::move(ret);
        }).finally([wp = std::move(wp)] {});
    } catch (...) {
        return futurize<return_type>::make_exception_future(std::current_exception());
    }
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
futurize_t<std::invoke_result_t<Func, Args...>>
async(Func&& func, Args&&... args) noexcept {
    return async(thread_attributes{}, std::forward<Func>(func), std::forward<Args>(args)...);
}
/// @}

SEASTAR_MODULE_EXPORT_END
}
