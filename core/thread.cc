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

#include "thread.hh"
#include "posix.hh"
#include <ucontext.h>
#include <algorithm>

/// \cond internal

namespace seastar {

thread_local jmp_buf_link g_unthreaded_context;
thread_local jmp_buf_link* g_current_context;

thread_context::thread_context(thread_attributes attr, std::function<void ()> func)
        : _attr(std::move(attr))
        , _func(std::move(func)) {
    setup();
    _all_threads.push_front(*this);
}

thread_context::~thread_context() {
    _all_threads.erase(_all_threads.iterator_to(*this));
}

std::unique_ptr<char[]>
thread_context::make_stack() {
    auto stack = std::make_unique<char[]>(_stack_size);
#ifdef ASAN_ENABLED
    // Avoid ASAN false positive due to garbage on stack
    std::fill_n(stack.get(), _stack_size, 0);
#endif
    return stack;
}

void
thread_context::setup() {
    // use setcontext() for the initial jump, as it allows us
    // to set up a stack, but continue with longjmp() as it's
    // much faster.
    ucontext_t initial_context;
    auto q = uint64_t(reinterpret_cast<uintptr_t>(this));
    auto main = reinterpret_cast<void (*)()>(&thread_context::s_main);
    auto r = getcontext(&initial_context);
    throw_system_error_on(r == -1);
    initial_context.uc_stack.ss_sp = _stack.get();
    initial_context.uc_stack.ss_size = _stack_size;
    initial_context.uc_link = nullptr;
    makecontext(&initial_context, main, 2, int(q), int(q >> 32));
    auto prev = g_current_context;
    _context.link = prev;
    _context.thread = this;
    g_current_context = &_context;
#ifdef ASAN_ENABLED
    swapcontext(&prev->context, &initial_context);
#else
    if (setjmp(prev->jmpbuf) == 0) {
        setcontext(&initial_context);
    }
#endif
}

void
thread_context::switch_in() {
    auto prev = g_current_context;
    g_current_context = &_context;
    _context.link = prev;
    if (_attr.scheduling_group) {
        _attr.scheduling_group->account_start();
        _context.yield_at = thread_clock::now() + _attr.scheduling_group->_this_period_remain;
    } else {
        _context.yield_at = {};
    }
#ifdef ASAN_ENABLED
    swapcontext(&prev->context, &_context.context);
#else
    if (setjmp(prev->jmpbuf) == 0) {
        longjmp(_context.jmpbuf, 1);
    }
#endif
}

void
thread_context::switch_out() {
    if (_attr.scheduling_group) {
        _attr.scheduling_group->account_stop();
    }
    g_current_context = _context.link;
#ifdef ASAN_ENABLED
    swapcontext(&_context.context, &g_current_context->context);
#else
    if (setjmp(_context.jmpbuf) == 0) {
        longjmp(g_current_context->jmpbuf, 1);
    }
#endif
}

bool
thread_context::should_yield() const {
    if (!_attr.scheduling_group) {
        return need_preempt();
    }
    return bool(_attr.scheduling_group->next_scheduling_point());
}

thread_local thread_context::preempted_thread_list thread_context::_preempted_threads;
thread_local thread_context::all_thread_list thread_context::_all_threads;

void
thread_context::yield() {
    if (!_attr.scheduling_group) {
        later().get();
    } else {
        auto when = _attr.scheduling_group->next_scheduling_point();
        if (when) {
            _preempted_threads.push_back(*this);
            _sched_promise.emplace();
            auto fut = _sched_promise->get_future();
            _sched_timer.arm(*when);
            fut.get();
            _sched_promise = stdx::nullopt;
        }
    }
}

bool thread::try_run_one_yielded_thread() {
    if (seastar::thread_context::_preempted_threads.empty()) {
        return false;
    }
    auto&& t = seastar::thread_context::_preempted_threads.front();
    t._sched_timer.cancel();
    t._sched_promise->set_value();
    thread_context::_preempted_threads.pop_front();
    return true;
}

void
thread_context::reschedule() {
    _preempted_threads.erase(_preempted_threads.iterator_to(*this));
    _sched_promise->set_value();
}

void
thread_context::s_main(unsigned int lo, unsigned int hi) {
    uintptr_t q = lo | (uint64_t(hi) << 32);
    reinterpret_cast<thread_context*>(q)->main();
}

void
thread_context::main() {
    if (_attr.scheduling_group) {
        _attr.scheduling_group->account_start();
    }
    try {
        _func();
        _done.set_value();
    } catch (...) {
        _done.set_exception(std::current_exception());
    }
    if (_attr.scheduling_group) {
        _attr.scheduling_group->account_stop();
    }
    g_current_context = _context.link;
#ifdef ASAN_ENABLED
    setcontext(&g_current_context->context);
#else
    longjmp(g_current_context->jmpbuf, 1);
#endif
}

namespace thread_impl {

void yield() {
    g_current_context->thread->yield();
}

void switch_in(thread_context* to) {
    to->switch_in();
}

void switch_out(thread_context* from) {
    from->switch_out();
}

void init() {
    g_unthreaded_context.link = nullptr;
    g_unthreaded_context.thread = nullptr;
    g_current_context = &g_unthreaded_context;
}

}

void thread::yield() {
    thread_impl::get()->yield();
}

bool thread::should_yield() {
    return thread_impl::get()->should_yield();
}

thread_scheduling_group::thread_scheduling_group(std::chrono::nanoseconds period, float usage)
        : _period(period), _quota(std::chrono::duration_cast<std::chrono::nanoseconds>(usage * period)) {
}

void
thread_scheduling_group::account_start() {
    auto now = thread_clock::now();
    if (now >= _this_period_ends) {
        _this_period_ends = now + _period;
        _this_period_remain = _quota;
    }
    _this_run_start = now;
}

void
thread_scheduling_group::account_stop() {
    _this_period_remain -= thread_clock::now() - _this_run_start;
}

stdx::optional<thread_clock::time_point>
thread_scheduling_group::next_scheduling_point() const {
    auto now = thread_clock::now();
    auto current_remain = _this_period_remain - (now - _this_run_start);
    if (current_remain > std::chrono::nanoseconds(0)) {
        return stdx::nullopt;
    }
    return _this_period_ends - current_remain;

}

}

/// \endcond
