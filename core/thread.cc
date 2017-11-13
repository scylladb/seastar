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

#ifdef ASAN_ENABLED

namespace {

#ifdef HAVE_ASAN_FIBER_SUPPORT
// ASan provides two functions as a means of informing it that user context
// switch has happened. First __sanitizer_start_switch_fiber() needs to be
// called with a place to store the fake stack pointer and the new stack
// information as arguments. Then, ucontext switch may be performed after which
// __sanitizer_finish_switch_fiber() needs to be called with a pointer to the
// current context fake stack and a place to store stack information of the
// previous ucontext.

extern "C" {
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* stack_bottom, size_t stack_size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** stack_bottom_old, size_t* stack_size_old);
}
#else
static inline void __sanitizer_start_switch_fiber(...) { }
static inline void __sanitizer_finish_switch_fiber(...) { }
#endif

thread_local jmp_buf_link* g_previous_context;

}

void jmp_buf_link::initial_switch_in(ucontext_t* initial_context, const void* stack_bottom, size_t stack_size)
{
    auto prev = std::exchange(g_current_context, this);
    link = prev;
    g_previous_context = prev;
    __sanitizer_start_switch_fiber(&prev->fake_stack, stack_bottom, stack_size);
    swapcontext(&prev->context, initial_context);
    __sanitizer_finish_switch_fiber(g_current_context->fake_stack, &g_previous_context->stack_bottom,
                                    &g_previous_context->stack_size);
}

void jmp_buf_link::switch_in()
{
    auto prev = std::exchange(g_current_context, this);
    link = prev;
    g_previous_context = prev;
    __sanitizer_start_switch_fiber(&prev->fake_stack, stack_bottom, stack_size);
    swapcontext(&prev->context, &context);
    __sanitizer_finish_switch_fiber(g_current_context->fake_stack, &g_previous_context->stack_bottom,
                                    &g_previous_context->stack_size);
}

void jmp_buf_link::switch_out()
{
    g_current_context = link;
    g_previous_context = this;
    __sanitizer_start_switch_fiber(&fake_stack, g_current_context->stack_bottom,
                                   g_current_context->stack_size);
    swapcontext(&context, &g_current_context->context);
    __sanitizer_finish_switch_fiber(g_current_context->fake_stack, &g_previous_context->stack_bottom,
                                    &g_previous_context->stack_size);
}

void jmp_buf_link::initial_switch_in_completed()
{
    // This is a new thread and it doesn't have the fake stack yet. ASan will
    // create it lazily, for now just pass nullptr.
    __sanitizer_finish_switch_fiber(nullptr, &g_previous_context->stack_bottom, &g_previous_context->stack_size);
}

void jmp_buf_link::final_switch_out()
{
    g_current_context = link;
    g_previous_context = this;
    // Since the thread is about to die we pass nullptr as fake_stack_save argument
    // so that ASan knows it can destroy the fake stack if it exists.
    __sanitizer_start_switch_fiber(nullptr, g_current_context->stack_bottom, g_current_context->stack_size);
    setcontext(&g_current_context->context);
}

#else

inline void jmp_buf_link::initial_switch_in(ucontext_t* initial_context, const void*, size_t)
{
    auto prev = std::exchange(g_current_context, this);
    link = prev;
    if (setjmp(prev->jmpbuf) == 0) {
        setcontext(initial_context);
    }
}

inline void jmp_buf_link::switch_in()
{
    auto prev = std::exchange(g_current_context, this);
    link = prev;
    if (setjmp(prev->jmpbuf) == 0) {
        longjmp(jmpbuf, 1);
    }
}

inline void jmp_buf_link::switch_out()
{
    g_current_context = link;
    if (setjmp(jmpbuf) == 0) {
        longjmp(g_current_context->jmpbuf, 1);
    }
}

inline void jmp_buf_link::initial_switch_in_completed()
{
}

inline void jmp_buf_link::final_switch_out()
{
    g_current_context = link;
    longjmp(g_current_context->jmpbuf, 1);
}

#endif

thread_context::thread_context(thread_attributes attr, std::function<void ()> func)
        : _attr(std::move(attr))
        , _func(std::move(func))
        , _scheduling_group(_attr.sched_group.value_or(current_scheduling_group())) {
    setup();
    _all_threads.push_front(*this);
}

thread_context::~thread_context() {
#ifdef SEASTAR_THREAD_STACK_GUARDS
    auto mp_result = mprotect(_stack.get(), getpagesize(), PROT_READ | PROT_WRITE);
    assert(mp_result == 0);
#endif
    _all_threads.erase(_all_threads.iterator_to(*this));
}

thread_context::stack_holder
thread_context::make_stack() {
#ifdef SEASTAR_THREAD_STACK_GUARDS
    auto stack = stack_holder(new (with_alignment(getpagesize())) char[_stack_size]);
#else
    auto stack = stack_holder(new char[_stack_size]);
#endif
#ifdef ASAN_ENABLED
    // Avoid ASAN false positive due to garbage on stack
    std::fill_n(stack.get(), _stack_size, 0);
#endif
    return stack;
}

void thread_context::stack_deleter::operator()(char* ptr) const noexcept {
#ifdef SEASTAR_THREAD_STACK_GUARDS
    operator delete[] (ptr, with_alignment(getpagesize()));
#else
    delete[] ptr;
#endif
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
#ifdef SEASTAR_THREAD_STACK_GUARDS
    size_t page_size = getpagesize();
    assert(align_up(_stack.get(), page_size) == _stack.get());
    assert(_stack_size > page_size * 4 && "Stack guard would take too much portion of the stack");
    auto mp_status = mprotect(_stack.get(), page_size, PROT_READ);
    throw_system_error_on(mp_status != 0, "mprotect");
#endif
    initial_context.uc_stack.ss_sp = _stack.get();
    initial_context.uc_stack.ss_size = _stack_size;
    initial_context.uc_link = nullptr;
    makecontext(&initial_context, main, 2, int(q), int(q >> 32));
    _context.thread = this;
    _context.initial_switch_in(&initial_context, _stack.get(), _stack_size);
}

void
thread_context::switch_in() {
    if (_attr.scheduling_group) {
        _attr.scheduling_group->account_start();
        _context.yield_at = _attr.scheduling_group->_this_run_start + _attr.scheduling_group->_this_period_remain;
    } else {
        _context.yield_at = {};
    }
    _context.switch_in();
}

void
thread_context::switch_out() {
    if (_attr.scheduling_group) {
        _attr.scheduling_group->account_stop();
    }
    _context.switch_out();
}

bool
thread_context::should_yield() const {
    if (!_attr.scheduling_group) {
        return need_preempt();
    }
    return need_preempt() || bool(_attr.scheduling_group->next_scheduling_point());
}

thread_local thread_context::preempted_thread_list thread_context::_preempted_threads;
thread_local thread_context::all_thread_list thread_context::_all_threads;

void
thread_context::yield() {
    if (!_attr.scheduling_group) {
        schedule(make_task(_scheduling_group, [this] {
            switch_in();
        }));
        switch_out();
    } else {
        auto when = _attr.scheduling_group->next_scheduling_point();
        if (when) {
            _preempted_threads.push_back(*this);
            _sched_promise.emplace();
            auto fut = _sched_promise->get_future();
            _sched_timer.arm(*when);
            fut.get();
            _sched_promise = stdx::nullopt;
        } else if (need_preempt()) {
            later().get();
        }
    }
}

bool thread::try_run_one_yielded_thread() {
    if (thread_context::_preempted_threads.empty()) {
        return false;
    }
    auto&& t = thread_context::_preempted_threads.front();
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
thread_context::s_main(int lo, int hi) {
    uintptr_t q = uint64_t(uint32_t(lo)) | uint64_t(hi) << 32;
    reinterpret_cast<thread_context*>(q)->main();
}

void
thread_context::main() {
#ifdef __x86_64__
    // There is no caller of main() in this context. We need to annotate this frame like this so that
    // unwinders don't try to trace back past this frame.
    // See https://github.com/scylladb/scylla/issues/1909.
    asm(".cfi_undefined rip");
#elif defined(__PPC__)
    asm(".cfi_undefined lr");
#else
    #warning "Backtracing from seastar threads may be broken"
#endif
    _context.initial_switch_in_completed();
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

    _context.final_switch_out();
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

scheduling_group
sched_group(const thread_context* thread) {
    return thread->_scheduling_group;
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
