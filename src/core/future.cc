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
 * Copyright (C) 2020 ScyllaDB
 */

#ifdef SEASTAR_MODULE
module;
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>
module seastar;
#else
#include <seastar/core/future.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/report_exception.hh>
#include <seastar/util/backtrace.hh>
#endif
#include <seastar/util/assert.hh>

namespace seastar {

// We can't test future_state_base directly because its private
// destructor is protected.
static_assert(std::is_nothrow_move_constructible_v<future_state<std::tuple<int>>>,
              "future_state's move constructor must not throw");

static_assert(sizeof(future_state<std::tuple<>>) <= 8, "future_state<std::tuple<>> is too large");
static_assert(sizeof(future_state<std::tuple<long>>) <= 16, "future_state<std::tuple<long>> is too large");
static_assert(future_state<std::tuple<>>::has_trivial_move_and_destroy, "future_state<std::tuple<>> not trivial");
static_assert(future_state<long>::has_trivial_move_and_destroy, "future_state<long> not trivial");

// We need to be able to move and copy std::exception_ptr in and out
// of future/promise/continuations without that producing a new
// exception.
static_assert(std::is_nothrow_copy_constructible_v<std::exception_ptr>,
    "std::exception_ptr's copy constructor must not throw");
static_assert(std::is_nothrow_move_constructible_v<std::exception_ptr>,
    "std::exception_ptr's move constructor must not throw");

namespace internal {

static_assert(std::is_empty_v<uninitialized_wrapper<std::tuple<>>>, "This should still be empty");

void promise_base::move_it(promise_base&& x) noexcept {
    // Don't use std::exchange to make sure x's values are nulled even
    // if &x == this.
    _task = x._task;
    x._task = nullptr;
#ifdef SEASTAR_DEBUG_PROMISE
    _task_shard = x._task_shard;
#endif
    _state = x._state;
    x._state = nullptr;
    _future = x._future;
    if (auto* fut = _future) {
        fut->detach_promise();
        fut->_promise = this;
    }
}

static void set_to_broken_promise(future_state_base& state) noexcept {
    try {
        // Constructing broken_promise may throw (std::logic_error ctor is not noexcept).
        state.set_exception(std::make_exception_ptr(broken_promise{}));
    } catch (...) {
        state.set_exception(std::current_exception());
    }
}

promise_base::promise_base(promise_base&& x) noexcept {
    move_it(std::move(x));
}

void promise_base::clear() noexcept {
    if (__builtin_expect(bool(_task), false)) {
        SEASTAR_ASSERT(_state && !_state->available());
        set_to_broken_promise(*_state);
        ::seastar::schedule(std::exchange(_task, nullptr));
    }
    if (_future) {
        SEASTAR_ASSERT(_state);
        if (!_state->available()) {
            set_to_broken_promise(*_state);
        }
        _future->detach_promise();
    }
}

promise_base& promise_base::operator=(promise_base&& x) noexcept {
    clear();
    move_it(std::move(x));
    return *this;
}

void promise_base::set_to_current_exception() noexcept {
    set_exception(std::current_exception());
}

#ifdef SEASTAR_DEBUG_PROMISE

void promise_base::assert_task_shard() const noexcept {
    if (_task_shard >= 0 && static_cast<shard_id>(_task_shard) != this_shard_id()) {
        on_fatal_internal_error(seastar_logger, format("Promise task was set on shard {} but made ready on shard {}", _task_shard, this_shard_id()));
    }
}

#endif

template <promise_base::urgent Urgent>
void promise_base::make_ready() noexcept {
    if (_task) {
        assert_task_shard();
        if (Urgent == urgent::yes) {
            ::seastar::schedule_urgent(std::exchange(_task, nullptr));
        } else {
            ::seastar::schedule(std::exchange(_task, nullptr));
        }
    }
}

template void promise_base::make_ready<promise_base::urgent::no>() noexcept;
template void promise_base::make_ready<promise_base::urgent::yes>() noexcept;
}

template
future<void> current_exception_as_future() noexcept;

/**
 * engine_exit() exits the reactor. It should be given a pointer to the
 * exception which prompted this exit - or a null pointer if the exit
 * request was not caused by any exception.
 */
void engine_exit(std::exception_ptr eptr) {
    if (!eptr) {
        engine().exit(0);
        return;
    }
    report_exception("Exiting on unhandled exception", eptr);
    engine().exit(1);
}

broken_promise::broken_promise() : logic_error("broken promise") { }

future_state_base::future_state_base(current_exception_future_marker) noexcept
    : future_state_base(std::current_exception()) { }

void future_state_base::ignore() noexcept {
    switch (_u.st) {
    case state::invalid:
    case state::future:
    case state::result_unavailable:
        SEASTAR_ASSERT(0 && "invalid state for ignore");
    case state::result:
        _u.st = state::result_unavailable;
        break;
    default:
        // Ignore the exception
        _u.take_exception();
    }
}

nested_exception::nested_exception(std::exception_ptr inner, std::exception_ptr outer) noexcept
    : inner(std::move(inner)), outer(std::move(outer)) {}

nested_exception::nested_exception(nested_exception&&) noexcept = default;

nested_exception::nested_exception(const nested_exception&) noexcept = default;

const char* nested_exception::what() const noexcept {
    return "seastar::nested_exception";
}

[[noreturn]] void nested_exception::rethrow_nested() const {
    std::rethrow_exception(outer);
}

static std::exception_ptr make_nested(std::exception_ptr&& inner, future_state_base&& old) noexcept {
    std::exception_ptr outer = std::move(old).get_exception();
    nested_exception nested{std::move(inner), std::move(outer)};
    return std::make_exception_ptr<nested_exception>(std::move(nested));
}

future_state_base::future_state_base(nested_exception_marker, future_state_base&& n, future_state_base&& old) noexcept {
    std::exception_ptr inner = std::move(n).get_exception();
    if (!old.failed()) {
        new (this) future_state_base(std::move(inner));
    } else {
        new (this) future_state_base(make_nested(std::move(inner), std::move(old)));
    }
}

future_state_base::future_state_base(nested_exception_marker, future_state_base&& old) noexcept {
    if (!old.failed()) {
        new (this) future_state_base(current_exception_future_marker());
        return;
    } else {
        new (this) future_state_base(make_nested(std::current_exception(), std::move(old)));
    }
}

void future_state_base::rethrow_exception() && {
    // Move ex out so future::~future() knows we've handled it
    std::rethrow_exception(std::move(*this).get_exception());
}

void future_state_base::rethrow_exception() const& {
    std::rethrow_exception(_u.ex);
}

void report_failed_future(const std::exception_ptr& eptr) noexcept {
    ++engine()._abandoned_failed_futures;
    seastar_logger.warn("Exceptional future ignored: {}, backtrace: {}", eptr, current_backtrace());
}

void report_failed_future(const future_state_base& state) noexcept {
    report_failed_future(state._u.ex);
}

void report_failed_future(future_state_base::any&& state) noexcept {
    report_failed_future(std::move(state).take_exception());
}

void reactor::test::with_allow_abandoned_failed_futures(unsigned count, noncopyable_function<void ()> func) {
    auto before = engine()._abandoned_failed_futures;
    auto old_level = seastar_logger.level();
    seastar_logger.set_level(log_level::error);
    func();
    auto after = engine()._abandoned_failed_futures;
    SEASTAR_ASSERT(after - before == count);
    engine()._abandoned_failed_futures = before;
    seastar_logger.set_level(old_level);
}

namespace {
class thread_wake_task final : public task {
    thread_context* _thread;
public:
    thread_wake_task(thread_context* thread) noexcept : _thread(thread) {}
    virtual void run_and_dispose() noexcept override {
        thread_impl::switch_in(_thread);
        // no need to delete, since this is always allocated on
        // _thread's stack.
    }
    /// Returns the task which is waiting for this thread to be done, or nullptr.
    virtual task* waiting_task() noexcept override {
        return _thread->waiting_task();
    }
};
}

void internal::future_base::do_wait() noexcept {
    auto thread = thread_impl::get();
    SEASTAR_ASSERT(thread);
    thread_wake_task wake_task{thread};
    wake_task.make_backtrace();
    _promise->set_task(&wake_task);
    thread_impl::switch_out(thread);
}

void internal::future_base::set_coroutine(task& coroutine) noexcept {
    SEASTAR_ASSERT(_promise);
    _promise->set_task(&coroutine);
}

}
