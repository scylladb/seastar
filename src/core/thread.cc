// If _FORTIFY_SOURCE is defined then longjmp ends up using longjmp_chk
// which asserts that you're jumping to the same stack. However, here we
// are intentionally switching stacks when longjmp'ing, so undefine this
// option to always use normal longjmp.
#undef _FORTIFY_SOURCE
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
#ifdef SEASTAR_MODULE
module;
#endif

#include <ucontext.h>
#ifndef SEASTAR_ASAN_ENABLED
#include <setjmp.h>
#endif
#include <stdint.h>
#include <valgrind/valgrind.h>
#include <exception>
#include <utility>
#include <boost/intrusive/list.hpp>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/thread.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/reactor.hh>
#include <seastar/util/assert.hh>
#endif

/// \cond internal

namespace seastar {

thread_local jmp_buf_link g_unthreaded_context;
thread_local jmp_buf_link* g_current_context;

#ifdef SEASTAR_ASAN_ENABLED

namespace {

#ifdef SEASTAR_HAVE_ASAN_FIBER_SUPPORT
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

// Both asan and optimizations can increase the stack used by a
// function. When both are used, we need more than 128 KiB.
#if defined(SEASTAR_ASAN_ENABLED)
static constexpr size_t base_stack_size = 256 * 1024;
#else
static constexpr size_t base_stack_size = 128 * 1024;
#endif

static size_t get_stack_size(thread_attributes attr) {
#if defined(__OPTIMIZE__) && defined(SEASTAR_ASAN_ENABLED)
    return std::max(base_stack_size, attr.stack_size);
#else
    return attr.stack_size ? attr.stack_size : base_stack_size;
#endif
}

thread_context::thread_context(thread_attributes attr, noncopyable_function<void ()> func)
        : task(attr.sched_group.value_or(current_scheduling_group()))
        , _stack(make_stack(get_stack_size(attr)))
        , _func(std::move(func)) {
    setup(get_stack_size(attr));
    _all_threads.push_front(*this);
}

thread_context::~thread_context() {
#ifdef SEASTAR_THREAD_STACK_GUARDS
    auto mp_result = mprotect(_stack.get(), getpagesize(), PROT_READ | PROT_WRITE);
    SEASTAR_ASSERT(mp_result == 0);
#endif
    _all_threads.erase(_all_threads.iterator_to(*this));
}

thread_context::stack_deleter::stack_deleter(int valgrind_id) : valgrind_id(valgrind_id) {}

thread_context::stack_holder
thread_context::make_stack(size_t stack_size) {
#ifdef SEASTAR_THREAD_STACK_GUARDS
    size_t page_size = getpagesize();
    size_t alignment = page_size;
#else
    size_t alignment = 16; // ABI requirement on x86_64
#endif
    void* mem = ::aligned_alloc(alignment, stack_size);
    if (mem == nullptr) {
        throw std::bad_alloc();
    }
    int valgrind_id = VALGRIND_STACK_REGISTER(mem, reinterpret_cast<char*>(mem) + stack_size);
    auto stack = stack_holder(new (mem) char[stack_size], stack_deleter(valgrind_id));
#ifdef SEASTAR_ASAN_ENABLED
    // Avoid ASAN false positive due to garbage on stack
    std::memset(stack.get(), 0, stack_size);
#endif

#ifdef SEASTAR_THREAD_STACK_GUARDS
    auto mp_status = mprotect(stack.get(), page_size, PROT_READ);
    throw_system_error_on(mp_status != 0, "mprotect");
#endif

    return stack;
}

void thread_context::stack_deleter::operator()(char* ptr) const noexcept {
    VALGRIND_STACK_DEREGISTER(valgrind_id);
    free(ptr);
}

void
thread_context::setup(size_t stack_size) {
    // use setcontext() for the initial jump, as it allows us
    // to set up a stack, but continue with longjmp() as it's
    // much faster.
    ucontext_t initial_context;
    auto q = uint64_t(reinterpret_cast<uintptr_t>(this));
    auto main = reinterpret_cast<void (*)()>(&thread_context::s_main);
    auto r = getcontext(&initial_context);
    throw_system_error_on(r == -1);
    initial_context.uc_stack.ss_sp = _stack.get();
    initial_context.uc_stack.ss_size = stack_size;
    initial_context.uc_link = nullptr;
    makecontext(&initial_context, main, 2, int(q), int(q >> 32));
    _context.thread = this;
    _context.initial_switch_in(&initial_context, _stack.get(), stack_size);
}

void
thread_context::switch_in() {
    local_engine->_current_task = nullptr; // thread_wake_task is on the stack and will be invalid when we resume
    _context.switch_in();
}

void
thread_context::switch_out() {
    _context.switch_out();
}

bool
thread_context::should_yield() const {
    return need_preempt();
}

thread_local thread_context::all_thread_list thread_context::_all_threads;

void
thread_context::run_and_dispose() noexcept {
    switch_in();
}

void
thread_context::yield() {
    schedule(this);
    switch_out();
}

void
thread_context::reschedule() {
    schedule(this);
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
#elif defined(__aarch64__)
    asm(".cfi_undefined x30");
#elif defined(__s390x__)
    asm(".cfi_undefined %r14");
#else
    #warning "Backtracing from seastar threads may be broken"
#endif
    _context.initial_switch_in_completed();
    if (group() != current_scheduling_group()) {
        yield();
    }
    try {
        _func();
        _done.set_value();
    } catch (...) {
        _done.set_exception(std::current_exception());
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
    return thread->group();
}

}

void thread::yield() {
    thread_impl::get()->yield();
}

}

/// \endcond
