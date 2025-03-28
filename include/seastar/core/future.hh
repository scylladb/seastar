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
#include <cassert>
#include <concepts>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#endif

#include <seastar/core/task.hh>
#include <seastar/core/thread_impl.hh>
#include <seastar/core/function_traits.hh>
#include <seastar/core/shard_id.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/critical_alloc_section.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/modules.hh>

namespace seastar {

struct nested_exception : public std::exception {
    std::exception_ptr inner;
    std::exception_ptr outer;
    nested_exception(std::exception_ptr inner, std::exception_ptr outer) noexcept;
    nested_exception(nested_exception&&) noexcept;
    nested_exception(const nested_exception&) noexcept;
    [[noreturn]] void rethrow_nested() const;
    virtual const char* what() const noexcept override;
};

/// \defgroup future-module Futures and Promises
///
/// \brief
/// Futures and promises are the basic tools for asynchronous
/// programming in seastar.  A future represents a result that
/// may not have been computed yet, for example a buffer that
/// is being read from the disk, or the result of a function
/// that is executed on another cpu.  A promise object allows
/// the future to be eventually resolved by assigning it a value.
///
/// \brief
/// Another way to look at futures and promises are as the reader
/// and writer sides, respectively, of a single-item, single use
/// queue.  You read from the future, and write to the promise,
/// and the system takes care that it works no matter what the
/// order of operations is.
///
/// \brief
/// The normal way of working with futures is to chain continuations
/// to them.  A continuation is a block of code (usually a lamdba)
/// that is called when the future is assigned a value (the future
/// is resolved); the continuation can then access the actual value.
///

/// \defgroup future-module-impl Implementation overview
/// \ingroup future-module
///
/// A future has a stored value. Semantically, the value is a
/// std::optional<std::variant<T, std::exception_ptr>>. The actual
/// type of the value in the implementation is future_state<T>.
///
/// A future without an initial value can be created by first creating
/// a promise and then calling promise::get_future. The promise also
/// stores a future_state<T> in case promise::set_value is called
/// before get_future.
///
/// In addition to the future_state<T>, the promise and the future
/// point to each other and the pointers are updated when either is
/// moved.
///
/// If a future is consumed by future::then before the future is
/// ready, a continuation is dynamically allocated. The continuation
/// also has a future_state<T>, but unlinke a future it is never
/// moved.
///
/// After a future creates a continuation, the corresponding promise
/// points to the newly allocated continuation. When
/// promise::set_value is called, the continuation is ready and is
/// scheduled.
///
/// A promise then consists of
/// * A future_state<T> for use when there is no corresponding future
///   or continuation (_local_state).
/// * A pointer to a future to allow updates when the promise is moved
///  (_future).
/// * A pointer to the continuation (_task).
/// * A pointer to future_state<T> (_state) that can point to
///   1. The future_state<T> in the promise itself
///   2. The future_state<T> in the future
///   2. The future_state<T> in the continuation
///
/// A special case is when a future blocks inside a thread. In that
/// case we still need a continuation, but that continuation doesn't
/// need a future_state<T> since the original future still exists on
/// the stack.
///
/// So the valid states for a promise are:
///
/// 1. A newly created promise. _state points to _local_state and
///    _task and _future are null.
/// 2. After get_future is called. _state points to the state in the
///    future, _future points to the future and _task is null.
/// 3. The future has been consumed by future::then. Now the _state
///    points to the state in the continuation, _future is null and
///    _task points to the continuation.
/// 4. A call to future::get is blocked in a thread. This is a mix of
///    cases 2 and 3. Like 2, there is a valid future and _future and
///    _state point to the future and its state. Like 3, there is a
///    valid continuation and _task points to it, but that
///    continuation has no state of its own.

/// \defgroup future-util Future Utilities
/// \ingroup future-module
///
/// \brief
/// These utilities are provided to help perform operations on futures.


/// \addtogroup future-module
/// @{
SEASTAR_MODULE_EXPORT_BEGIN
template <class T = void>
class promise;

template <class T>
class future;

template <typename... T>
class shared_future;

struct future_state_base;

/// \brief Creates a \ref future in an available, value state.
///
/// Creates a \ref future object that is already resolved.  This
/// is useful when it is determined that no I/O needs to be performed
/// to perform a computation (for example, because the data is cached
/// in some buffer).
template <typename T = void, typename... A>
future<T> make_ready_future(A&&... value) noexcept;


/// \brief Returns a ready \ref future that is already resolved.
template<typename T>
inline
future<std::remove_cv_t<std::remove_reference_t<T>>> as_ready_future(T&& v) noexcept {
    return make_ready_future<std::remove_cv_t<std::remove_reference_t<T>>>(
        std::forward<T>(v));
}

/// \brief Creates a \ref future in an available, failed state.
///
/// Creates a \ref future object that is already resolved in a failed
/// state.  This is useful when no I/O needs to be performed to perform
/// a computation (for example, because the connection is closed and
/// we cannot read from it).
template <typename T = void>
future<T> make_exception_future(std::exception_ptr&& value) noexcept;

template <typename T = void, typename Exception>
future<T> make_exception_future(Exception&& ex) noexcept;

template <typename T = void>
future<T> make_exception_future(const std::exception_ptr& ex) noexcept {
    return make_exception_future<T>(std::exception_ptr(ex));
}

template <typename T = void>
future<T> make_exception_future(std::exception_ptr& ex) noexcept {
    return make_exception_future<T>(static_cast<const std::exception_ptr&>(ex));
}

template <typename T = void>
future<T> make_exception_future(const std::exception_ptr&& ex) noexcept {
    // as ex is const, we cannot move it, but can copy it.
    return make_exception_future<T>(std::exception_ptr(ex));
}
SEASTAR_MODULE_EXPORT_END
/// \cond internal
void engine_exit(std::exception_ptr eptr = {});

/// \endcond

/// \brief Exception type for broken promises
///
/// When a promise is broken, i.e. a promise object with an attached
/// continuation is destroyed before setting any value or exception, an
/// exception of `broken_promise` type is propagated to that abandoned
/// continuation.
SEASTAR_MODULE_EXPORT
struct broken_promise : std::logic_error {
    broken_promise();
};

/// \brief Returns std::current_exception() wrapped in a future
///
/// This is equivalent to
/// make_exception_future(std::current_exception()), but expands to
/// less code.
SEASTAR_MODULE_EXPORT
template <typename T = void>
future<T> current_exception_as_future() noexcept;

SEASTAR_MODULE_EXPORT
extern template
future<void> current_exception_as_future() noexcept;

namespace internal {
template <class T = void>
class promise_base_with_type;
class promise_base;

struct monostate {};

template <typename... T>
struct future_stored_type;

template <>
struct future_stored_type<> {
    using type = monostate;
};

template <typename T>
struct future_stored_type<T> {
    using type = std::conditional_t<std::is_void_v<T>, internal::monostate, T>;
};

template <typename... T>
using future_stored_type_t = typename future_stored_type<T...>::type;

template<typename T>
using future_tuple_type_t = std::conditional_t<std::is_same_v<T, monostate>, std::tuple<>, std::tuple<T>>;

// It doesn't seem to be possible to use std::tuple_element_t with an empty tuple. There is an static_assert in it that
// fails the build even if it is in the non enabled side of std::conditional.
template <typename T>
struct get0_return_type;

template <>
struct get0_return_type<internal::monostate> {
    using type = void;
    static type get0(internal::monostate) { }
};

template <typename T>
struct get0_return_type {
    using type = T;
    static T get0(T&& v) { return std::move(v); }
};

template<typename T>
using maybe_wrap_ref = std::conditional_t<std::is_reference_v<T>, std::reference_wrapper<std::remove_reference_t<T>>, T>;

/// \brief Wrapper for keeping uninitialized values of non default constructible types.
///
/// This is similar to a std::optional<T>, but it doesn't know if it is holding a value or not, so the user is
/// responsible for calling constructors and destructors.

template <typename T>
struct uninitialized_wrapper {
    using tuple_type = future_tuple_type_t<T>;
    [[no_unique_address]] union any {
        any() noexcept {}
        ~any() {}
        // T can be a reference, so wrap it.
        [[no_unique_address]] maybe_wrap_ref<T> value;
    } _v;

public:
    uninitialized_wrapper() noexcept = default;
    template<typename... U>
    std::enable_if_t<!std::is_same_v<std::tuple<std::remove_cv_t<U>...>, std::tuple<tuple_type>>, void>
    uninitialized_set(U&&... vs) {
        new (&_v.value) maybe_wrap_ref<T>(T(std::forward<U>(vs)...));
    }
    void uninitialized_set(tuple_type&& v) {
        uninitialized_set(std::move(std::get<0>(v)));
    }
    void uninitialized_set(const tuple_type& v) {
        uninitialized_set(std::get<0>(v));
    }
    maybe_wrap_ref<T>& uninitialized_get() {
        return _v.value;
    }
    const maybe_wrap_ref<T>& uninitialized_get() const {
        return _v.value;
    }
};

template <>
struct uninitialized_wrapper<internal::monostate> {
    [[no_unique_address]] internal::monostate _v;
public:
    uninitialized_wrapper() noexcept = default;
    void uninitialized_set() {
    }
    void uninitialized_set(internal::monostate) {
    }
    void uninitialized_set(std::tuple<>&& v) {
    }
    void uninitialized_set(const std::tuple<>& v) {
    }
    internal::monostate& uninitialized_get() {
        return _v;
    }
    const internal::monostate& uninitialized_get() const {
        return _v;
    }
};

template <typename T>
struct is_trivially_move_constructible_and_destructible {
    static constexpr bool value = std::is_trivially_move_constructible_v<T> && std::is_trivially_destructible_v<T>;
};

template <bool... v>
struct all_true : std::false_type {};

template <>
struct all_true<> : std::true_type {};

template <bool... v>
struct all_true<true, v...> : public all_true<v...> {};

template<typename T>
struct is_tuple_effectively_trivially_move_constructible_and_destructible_helper;

template <typename... T>
struct is_tuple_effectively_trivially_move_constructible_and_destructible_helper<std::tuple<T...>> {
    static constexpr bool value = all_true<is_trivially_move_constructible_and_destructible<T>::value...>::value;
};

template <typename T>
static constexpr bool is_tuple_effectively_trivially_move_constructible_and_destructible =
    is_tuple_effectively_trivially_move_constructible_and_destructible_helper<T>::value;

}

//
// A future/promise pair maintain one logical value (a future_state).
// There are up to three places that can store it, but only one is
// active at any time.
//
// - in the promise _local_state member variable
//
//   This is necessary because a promise is created first and there
//   would be nowhere else to put the value.
//
// - in the future _state variable
//
//   This is used anytime a future exists and then has not been called
//   yet. This guarantees a simple access to the value for any code
//   that already has a future.
//
// - in the task associated with the .then() clause (after .then() is called,
//   if a value was not set)
//
//
// The promise maintains a pointer to the state, which is modified as
// the state moves to a new location due to events (such as .then() or
// get_future being called) or due to the promise or future being
// moved around.
//

// non templated base class to reduce code duplication
SEASTAR_MODULE_EXPORT
struct future_state_base {
    static_assert(sizeof(std::exception_ptr) == sizeof(void*), "exception_ptr not a pointer");
    enum class state : uintptr_t {
         invalid = 0,
         future = 1,
         // the substate is intended to decouple the run-time prevention
         // for duplicative result extraction (calling e.g. then() twice
         // ends up in abandoned()) from the wrapped object's destruction
         // handling which is orchestrated by future_state. Instead of
         // creating a temporary future_state just for the sake of setting
         // the "invalid" in the source instance, result_unavailable can
         // be set to ensure future_state_base::available() returns false.
         result_unavailable = 2,
         result = 3,
         exception_min = 4,  // or anything greater
    };
    union any {
        any() noexcept { st = state::future; }
        any(state s) noexcept { st = s; }
        void set_exception(std::exception_ptr&& e) noexcept {
            new (&ex) std::exception_ptr(std::move(e));
            SEASTAR_ASSERT(st >= state::exception_min);
        }
        any(std::exception_ptr&& e) noexcept {
            set_exception(std::move(e));
        }
        // From a users' perspective, a result_unavailable is not valid
        bool valid() const noexcept { return st != state::invalid && st != state::result_unavailable; }
        bool available() const noexcept { return st == state::result || st >= state::exception_min; }
        bool failed() const noexcept { return __builtin_expect(st >= state::exception_min, false); }
        void check_failure() noexcept;
        ~any() noexcept { }
        std::exception_ptr take_exception() noexcept {
            std::exception_ptr ret(std::move(ex));
            // Unfortunately in libstdc++ ~exception_ptr is defined out of line. We know that it does nothing for
            // moved out values, so we omit calling it. This is critical for the code quality produced for this
            // function. Without the out of line call, gcc can figure out that both sides of the if produce
            // identical code and merges them.if
            // We don't make any assumptions about other c++ libraries.
            // There is request with gcc to define it inline: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=90295
#ifndef __GLIBCXX__
            ex.~exception_ptr();
#endif
            st = state::invalid;
            return ret;
        }
        void move_it(any&& x) noexcept {
#ifdef __GLIBCXX__
            // Unfortunally gcc cannot fully optimize the regular
            // implementation:
            // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=95014
            // Given what we know about the libstdc++ implementation
            // (see the comment in take_exception), we can just
            // memmove and zero x.  We use memmove to guarantee
            // vaild results if &x == this.
            memmove(static_cast<void*>(this), &x, sizeof(any));
            x.st = state::invalid;
#else
            if (x.st < state::exception_min) {
                st = x.st;
                x.st = state::invalid;
            } else {
                new (&ex) std::exception_ptr(x.take_exception());
            }
#endif
        }
        any(any&& x) noexcept {
            move_it(std::move(x));
        }
        any& operator=(any&& x) noexcept {
            check_failure();
            // If this is a self move assignment, check_failure
            // guarantees that we don't have an exception and calling
            // move_it is safe.
            move_it(std::move(x));
            return *this;
        }
        bool has_result() const noexcept {
            return st == state::result || st == state::result_unavailable;
        }
        state st;
        std::exception_ptr ex;
    } _u;

    future_state_base() noexcept = default;
    future_state_base(state st) noexcept : _u(st) { }
    future_state_base(std::exception_ptr&& ex) noexcept : _u(std::move(ex)) { }
    future_state_base(future_state_base&& x) noexcept : _u(std::move(x._u)) { }

    // We never need to destruct this polymorphicly, so we can make it
    // protected instead of virtual.
protected:
    struct current_exception_future_marker {};
    future_state_base(current_exception_future_marker) noexcept;
    struct nested_exception_marker {};
    future_state_base(nested_exception_marker, future_state_base&& old) noexcept;
    future_state_base(nested_exception_marker, future_state_base&& n, future_state_base&& old) noexcept;
    ~future_state_base() noexcept = default;

    void rethrow_exception() &&;
    void rethrow_exception() const&;

public:

    bool valid() const noexcept { return _u.valid(); }
    bool available() const noexcept { return _u.available(); }
    bool failed() const noexcept { return _u.failed(); }

    void ignore() noexcept;

    void set_exception(std::exception_ptr&& ex) noexcept {
        SEASTAR_ASSERT(_u.st == state::future);
        _u.set_exception(std::move(ex));
    }
    future_state_base& operator=(future_state_base&& x) noexcept = default;
    void set_exception(future_state_base&& state) noexcept {
        SEASTAR_ASSERT(_u.st == state::future);
        *this = std::move(state);
    }
    std::exception_ptr get_exception() && noexcept {
        SEASTAR_ASSERT(_u.st >= state::exception_min);
        // Move ex out so future::~future() knows we've handled it
        return _u.take_exception();
    }
    const std::exception_ptr& get_exception() const& noexcept {
        SEASTAR_ASSERT(_u.st >= state::exception_min);
        return _u.ex;
    }
    template <typename U>
    friend struct future_state;
    template <typename U>
    friend future<U> current_exception_as_future() noexcept;
    template <typename U>
    friend class future;
    template <typename T>
    friend struct futurize;
};

namespace internal {
void report_failed_future(const std::exception_ptr& ex) noexcept;
void report_failed_future(const future_state_base& state) noexcept;
void report_failed_future(future_state_base::any&& state) noexcept;
} // internal namespace


inline void future_state_base::any::check_failure() noexcept {
    if (failed()) {
        internal::report_failed_future(std::move(*this));
    }
}

struct ready_future_marker {};
struct exception_future_marker {};
struct future_for_get_promise_marker {};

/// \cond internal
template <typename T>
struct future_state :  public future_state_base, private internal::uninitialized_wrapper<T> {
    static constexpr bool copy_noexcept = std::is_nothrow_copy_constructible_v<T>;
    static constexpr bool has_trivial_move_and_destroy = internal::is_trivially_move_constructible_and_destructible<T>::value;
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "Types must be no-throw move constructible");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "Types must be no-throw destructible");
    future_state() noexcept = default;
    void move_it(future_state&& x) noexcept {
        if constexpr (has_trivial_move_and_destroy) {
#pragma GCC diagnostic push
            // This function may copy uninitialized memory, such as when
            // creating an uninitialized promise and calling get_future()
            // on it. Gcc 12 started to catch some simple cases of this
            // at compile time, so we need to tell it that it's fine.
#pragma GCC diagnostic ignored "-Wuninitialized"
            memmove(reinterpret_cast<char*>(&this->uninitialized_get()),
                   &x.uninitialized_get(),
                   internal::used_size<internal::maybe_wrap_ref<T>>::value);
#pragma GCC diagnostic pop
        } else if (_u.has_result()) {
            this->uninitialized_set(std::move(x.uninitialized_get()));
            std::destroy_at(&x.uninitialized_get());
        }
    }

    [[gnu::always_inline]]
    future_state(future_state&& x) noexcept : future_state_base(std::move(x)) {
        move_it(std::move(x));
    }

    void clear() noexcept {
        if (_u.has_result()) {
            std::destroy_at(&this->uninitialized_get());
        } else {
            _u.check_failure();
        }
    }
    __attribute__((always_inline))
    ~future_state() noexcept {
        clear();
    }
    future_state& operator=(future_state&& x) noexcept {
        clear();
        future_state_base::operator=(std::move(x));
        // If &x == this, _u.st is now state::invalid and so it is
        // safe to call move_it.
        move_it(std::move(x));
        return *this;
    }
    template <typename... A>
    future_state(ready_future_marker, A&&... a) noexcept : future_state_base(state::result) {
      try {
        this->uninitialized_set(std::forward<A>(a)...);
      } catch (...) {
        new (this) future_state(current_exception_future_marker());
      }
    }
    template <typename... A>
    void set(A&&... a) noexcept {
        SEASTAR_ASSERT(_u.st == state::future);
        new (this) future_state(ready_future_marker(), std::forward<A>(a)...);
    }
    future_state(exception_future_marker, std::exception_ptr&& ex) noexcept : future_state_base(std::move(ex)) { }
    future_state(exception_future_marker, future_state_base&& state) noexcept : future_state_base(std::move(state)) { }
    future_state(current_exception_future_marker m) noexcept : future_state_base(m) { }
    future_state(nested_exception_marker m, future_state_base&& old) noexcept : future_state_base(m, std::move(old)) { }
    future_state(nested_exception_marker m, future_state_base&& n, future_state_base&& old) noexcept : future_state_base(m, std::move(n), std::move(old)) { }
    T&& get_value() && noexcept {
        SEASTAR_ASSERT(_u.st == state::result);
        return static_cast<T&&>(this->uninitialized_get());
    }
    T&& take_value() && noexcept {
        SEASTAR_ASSERT(_u.st == state::result);
        _u.st = state::result_unavailable;
        return static_cast<T&&>(this->uninitialized_get());
    }
    template<typename U = T>
    const std::enable_if_t<std::is_copy_constructible_v<U>, U>& get_value() const& noexcept(copy_noexcept) {
        SEASTAR_ASSERT(_u.st == state::result);
        return this->uninitialized_get();
    }
    T&& take() && {
        SEASTAR_ASSERT(available());
        if (_u.st >= state::exception_min) {
            std::move(*this).rethrow_exception();
        }
        _u.st = state::result_unavailable;
        return static_cast<T&&>(this->uninitialized_get());
    }
    T&& get() && {
        SEASTAR_ASSERT(available());
        if (_u.st >= state::exception_min) {
            std::move(*this).rethrow_exception();
        }
        return static_cast<T&&>(this->uninitialized_get());
    }
    const T& get() const& {
        SEASTAR_ASSERT(available());
        if (_u.st >= state::exception_min) {
            rethrow_exception();
        }
        return this->uninitialized_get();
    }
    using get0_return_type = typename internal::get0_return_type<T>::type;
    static get0_return_type get0(T&& x) {
        return internal::get0_return_type<T>::get0(std::move(x));
    }

    get0_return_type get0() {
        return std::move(*this).get();
    }
};

template <typename T = void>
class continuation_base : public task {
protected:
    using future_state = seastar::future_state<internal::future_stored_type_t<T>>;
    future_state _state;
    using future_type = future<T>;
    using promise_type = promise<T>;
public:
    continuation_base() noexcept = default;
    void set_state(future_state&& state) noexcept {
        _state = std::move(state);
    }
    // This override of waiting_task() is needed here because there are cases
    // when backtrace is obtained from the destructor of this class and objects
    // of derived classes are already destroyed at that time. If we didn't
    // have this override we would get a "pure virtual function call" exception.
    virtual task* waiting_task() noexcept override { return nullptr; }
    friend class internal::promise_base_with_type<T>;
    friend class promise<T>;
    friend class future<T>;
};

// Given a future type, find the corresponding continuation_base.
template <typename Future>
struct continuation_base_from_future;

template <typename... T>
struct continuation_base_from_future<future<T...>> {
    using type = continuation_base<T...>;
};

template <typename Future>
using continuation_base_from_future_t = typename continuation_base_from_future<Future>::type;

template <typename Promise, typename T = void>
class continuation_base_with_promise : public continuation_base<T> {
    friend class internal::promise_base_with_type<T>;
protected:
    continuation_base_with_promise(Promise&& pr) noexcept : _pr(std::move(pr)) {
        task::make_backtrace();
    }
    virtual task* waiting_task() noexcept override;
    Promise _pr;
};

template <typename Promise, typename Func, typename Wrapper, typename T = void>
struct continuation final : continuation_base_with_promise<Promise, T> {
    // Func is the original function passed to then/then_wrapped. The
    // Wrapper is a helper function that implements the specific logic
    // needed by then/then_wrapped. We call the wrapper passing it the
    // original function, promise and state.
    // Note that if Func's move constructor throws, this will call
    // std::unexpected. We could try to require Func to be nothrow
    // move constructible, but that will cause a lot of churn. Since
    // we can't support a failure to create a continuation, calling
    // std::unexpected as close to the failure as possible is the best
    // we can do.
    continuation(Promise&& pr, Func&& func, Wrapper&& wrapper) noexcept
        : continuation_base_with_promise<Promise, T>(std::move(pr))
        , _func(std::move(func))
        , _wrapper(std::move(wrapper)) {}
    virtual void run_and_dispose() noexcept override {
        try {
            _wrapper(std::move(this->_pr), _func, std::move(this->_state));
        } catch (...) {
            this->_pr.set_to_current_exception();
        }
        delete this;
    }
    Func _func;
    [[no_unique_address]] Wrapper _wrapper;
};

namespace internal {

template <typename T = void>
future<T> make_exception_future(future_state_base&& state) noexcept;

template <typename T = void>
void set_callback(future<T>&& fut, continuation_base<T>* callback) noexcept;

class future_base;

class promise_base {
protected:
    enum class urgent { no, yes };
    future_base* _future = nullptr;

    // This points to the future_state that is currently being
    // used. See comment above the future_state struct definition for
    // details.
    future_state_base* _state;

    task* _task = nullptr;
#ifdef SEASTAR_DEBUG_PROMISE
    int _task_shard = -1;

    void set_task(task* task) noexcept {
        _task = task;
        _task_shard = this_shard_id();
    }
    void assert_task_shard() const noexcept;
#else
    void set_task(task* task) noexcept {
        _task = task;
    }
    void assert_task_shard() const noexcept { }
#endif

    promise_base(const promise_base&) = delete;
    promise_base(future_state_base* state) noexcept : _state(state) {}
    promise_base(future_base* future, future_state_base* state) noexcept;
    void move_it(promise_base&& x) noexcept;
    promise_base(promise_base&& x) noexcept;

    void clear() noexcept;

    // We never need to destruct this polymorphicly, so we can make it
    // protected instead of virtual
    ~promise_base() noexcept {
        clear();
    }

    void operator=(const promise_base&) = delete;
    promise_base& operator=(promise_base&& x) noexcept;

    template<urgent Urgent>
    void make_ready() noexcept;

    template<typename T>
    void set_exception_impl(T&& val) noexcept {
        if (_state) {
            _state->set_exception(std::move(val));
            make_ready<urgent::no>();
        } else {
            // We get here if promise::get_future is called and the
            // returned future is destroyed without creating a
            // continuation.
            // In older versions of seastar we would store a local
            // copy of ex and warn in the promise destructor.
            // Since there isn't any way for the user to clear
            // the exception, we issue the warning from here.
            internal::report_failed_future(val);
        }
    }

    void set_exception(future_state_base&& state) noexcept {
        set_exception_impl(std::move(state));
    }

    void set_exception(std::exception_ptr&& ex) noexcept {
        set_exception_impl(std::move(ex));
    }

    void set_exception(const std::exception_ptr& ex) noexcept {
        set_exception(std::exception_ptr(ex));
    }

    template<typename Exception>
    std::enable_if_t<!std::is_same_v<std::remove_reference_t<Exception>, std::exception_ptr>, void> set_exception(Exception&& e) noexcept {
        set_exception(std::make_exception_ptr(std::forward<Exception>(e)));
    }

    friend class future_base;
    template <typename U> friend class seastar::future;

public:
    /// Set this promise to the current exception.
    ///
    /// This is equivalent to set_exception(std::current_exception()),
    /// but expands to less code.
    void set_to_current_exception() noexcept;

    /// Returns the task which is waiting for this promise to resolve, or nullptr.
    task* waiting_task() const noexcept { return _task; }
};

/// \brief A promise with type but no local data.
///
/// This is a promise without any local data. We use this for when the
/// future is created first, so we know the promise always has an
/// external place to point to. We cannot just use promise_base
/// because we need to know the type that is being stored.
template <typename T>
class promise_base_with_type : protected internal::promise_base {
protected:
    using future_state = seastar::future_state<future_stored_type_t<T>>;
    future_state* get_state() noexcept {
        return static_cast<future_state*>(_state);
    }
    static constexpr bool copy_noexcept = future_state::copy_noexcept;
public:
    promise_base_with_type(future_state_base* state) noexcept : promise_base(state) { }
    promise_base_with_type(future<T>* future) noexcept : promise_base(future, &future->_state) { }
    promise_base_with_type(promise_base_with_type&& x) noexcept = default;
    promise_base_with_type(const promise_base_with_type&) = delete;
    promise_base_with_type& operator=(promise_base_with_type&& x) noexcept = default;
    void operator=(const promise_base_with_type&) = delete;

    void set_urgent_state(future_state&& state) noexcept {
        auto* ptr = get_state();
        // The state can be null if the corresponding future has been
        // destroyed without producing a continuation.
        if (ptr) {
            // FIXME: This is a fairly expensive assert. It would be a
            // good candidate for being disabled in release builds if
            // we had such an assert.
            SEASTAR_ASSERT(ptr->_u.st == future_state_base::state::future);
            new (ptr) future_state(std::move(state));
            make_ready<urgent::yes>();
        }
    }

    template <typename... A>
    void set_value(A&&... a) noexcept {
        if (auto *s = get_state()) {
            s->set(std::forward<A>(a)...);
            make_ready<urgent::no>();
        }
    }

    /// Set this promise to the current exception.
    ///
    /// This is equivalent to set_exception(std::current_exception()),
    /// but expands to less code.
    void set_to_current_exception() noexcept {
        internal::promise_base::set_to_current_exception();
    }

    /// Returns the task which is waiting for this promise to resolve, or nullptr.
    using internal::promise_base::waiting_task;

private:

    template <typename U>
    friend class seastar::future;

    friend future_state;
};
}
/// \endcond

/// \brief promise - allows a future value to be made available at a later time.
///
/// \tparam T A type to be carried as the result of the associated future. Use void (default) for no result.
SEASTAR_MODULE_EXPORT
template <typename T>
class promise : private internal::promise_base_with_type<T> {
    using future_state = typename internal::promise_base_with_type<T>::future_state;
    future_state _local_state;

public:
    /// \brief Constructs an empty \c promise.
    ///
    /// Creates promise with no associated future yet (see get_future()).
    promise() noexcept : internal::promise_base_with_type<T>(&_local_state) {}

    /// \brief Moves a \c promise object.
    void move_it(promise&& x) noexcept;
    promise(promise&& x) noexcept : internal::promise_base_with_type<T>(std::move(x)) {
        move_it(std::move(x));
    }
    promise(const promise&) = delete;
    promise& operator=(promise&& x) noexcept {
        internal::promise_base_with_type<T>::operator=(std::move(x));
        // If this is a self-move, _state is now nullptr and it is
        // safe to call move_it.
        move_it(std::move(x));
        return *this;
    }
    void operator=(const promise&) = delete;

    /// Set this promise to the current exception.
    ///
    /// This is equivalent to set_exception(std::current_exception()),
    /// but expands to less code.
    void set_to_current_exception() noexcept {
        internal::promise_base::set_to_current_exception();
    }

    /// Returns the task which is waiting for this promise to resolve, or nullptr.
    using internal::promise_base::waiting_task;

    /// \brief Gets the promise's associated future.
    ///
    /// The future and promise will be remember each other, even if either or
    /// both are moved.  When \c set_value() or \c set_exception() are called
    /// on the promise, the future will be become ready, and if a continuation
    /// was attached to the future, it will run.
    future<T> get_future() noexcept;

    /// \brief Sets the promises value
    ///
    /// Forwards the arguments and makes them available to the associated
    /// future.  May be called either before or after \c get_future().
    ///
    /// The arguments can have either the types the promise is
    /// templated with, or a corresponding std::tuple. That is, given
    /// a promise<int, double>, both calls are valid:
    ///
    /// pr.set_value(42, 43.0);
    /// pr.set_value(std::tuple<int, double>(42, 43.0))
    template <typename... A>
    void set_value(A&&... a) noexcept {
        internal::promise_base_with_type<T>::set_value(std::forward<A>(a)...);
    }

    /// \brief Marks the promise as failed
    ///
    /// Forwards the exception argument to the future and makes it
    /// available.  May be called either before or after \c get_future().
    void set_exception(std::exception_ptr&& ex) noexcept {
        internal::promise_base::set_exception(std::move(ex));
    }

    void set_exception(const std::exception_ptr& ex) noexcept {
        internal::promise_base::set_exception(ex);
    }

    /// \brief Marks the promise as failed
    ///
    /// Forwards the exception argument to the future and makes it
    /// available.  May be called either before or after \c get_future().
    template<typename Exception>
    std::enable_if_t<!std::is_same_v<std::remove_reference_t<Exception>, std::exception_ptr>, void> set_exception(Exception&& e) noexcept {
        internal::promise_base::set_exception(std::forward<Exception>(e));
    }

    using internal::promise_base_with_type<T>::set_urgent_state;

    template <typename U>
    friend class future;
};

/// @}

/// \addtogroup future-util
/// @{


/// \brief Check whether a type is a future
///
/// This is a type trait evaluating to \c true if the given type is a
/// future.
///
template <typename... T> struct is_future : std::false_type {};

/// \cond internal
/// \addtogroup future-util
template <typename... T> struct is_future<future<T...>> : std::true_type {};

/// \endcond


/// \brief Converts a type to a future type, if it isn't already.
///
/// \return Result in member type 'type'.
SEASTAR_MODULE_EXPORT
template <typename T>
struct futurize;

template <typename T>
concept Future = is_future<T>::value;

template <typename Func, typename... T>
concept CanInvoke = std::invocable<Func, T...>;

template <typename Func, typename... T>
concept CanApplyTuple
    = sizeof...(T) == 1
        && requires (Func func, std::tuple<T...> wrapped_val) {
        { std::apply(func, std::get<0>(std::move(wrapped_val))) };
    };

template <typename Func, typename... T>
concept InvokeReturnsAnyFuture = Future<std::invoke_result_t<Func, T...>>;

/// \endcond

// Converts a type to a future type, if it isn't already.
template <typename T>
using futurize_t = typename futurize<T>::type;

/// @}

template<typename Func, typename... Args>
auto futurize_invoke(Func&& func, Args&&... args) noexcept;

template<typename Func, typename... Args>
auto futurize_apply(Func&& func, std::tuple<Args...>&& args) noexcept;

/// \addtogroup future-module
/// @{
namespace internal {
class future_base {
protected:
    promise_base* _promise;
    future_base() noexcept : _promise(nullptr) {}
    future_base(promise_base* promise, future_state_base* state) noexcept : _promise(promise) {
        _promise->_future = this;
        _promise->_state = state;
    }

    void move_it(future_base&& x, future_state_base* state) noexcept {
        _promise = x._promise;
        if (auto* p = _promise) {
            x.detach_promise();
            p->_future = this;
            p->_state = state;
        }
    }

    future_base(future_base&& x, future_state_base* state) noexcept {
        move_it(std::move(x), state);
    }

    void clear() noexcept {
        if (_promise) {
            detach_promise();
        }
    }

    ~future_base() noexcept {
        clear();
    }

    promise_base* detach_promise() noexcept {
        _promise->_state = nullptr;
        _promise->_future = nullptr;
        return std::exchange(_promise, nullptr);
    }

    void schedule(task* tws, future_state_base* state) noexcept {
        promise_base* p = detach_promise();
        p->_state = state;
        p->set_task(tws);
    }

    void do_wait() noexcept;

    void set_coroutine(task& coroutine) noexcept;

    friend class promise_base;
};

template <typename Func, typename... T>
struct future_result  {
    using type = std::invoke_result_t<Func, T...>;
    using future_type = futurize_t<type>;
    using func_type = future_type (T&&...);
};

template <typename Func>
struct future_result<Func, void> {
    using type = std::invoke_result_t<Func>;
    using future_type = futurize_t<type>;
    using func_type = future_type ();
};

template <typename Func, typename T>
using future_result_t = typename future_result<Func, T>::type;

template <typename Func, typename T>
auto future_invoke(Func&& func, T&& v) {
    if constexpr (std::is_same_v<T, monostate>) {
        return std::invoke(std::forward<Func>(func));
    } else {
        return std::invoke(std::forward<Func>(func), std::forward<T>(v));
    }
}

template <typename Func, typename... T>
struct result_of_apply {
    // no "type" member if not a function call signature or not a tuple
};

template <typename Func, typename... T>
struct result_of_apply<Func, std::tuple<T...>> : std::invoke_result<Func, T...> {
    // Let std::invoke_result_t determine the result if the input is a tuple
};

template <typename Func, typename... T>
using result_of_apply_t = typename result_of_apply<Func, T...>::type;

}

template <typename Promise, typename T>
task* continuation_base_with_promise<Promise, T>::waiting_task() noexcept {
    return _pr.waiting_task();
}

/// \brief A representation of a possibly not-yet-computed value.
///
/// A \c future represents a value that has not yet been computed
/// (an asynchronous computation).  It can be in one of several
/// states:
///    - unavailable: the computation has not been completed yet
///    - value: the computation has been completed successfully and a
///      value is available.
///    - failed: the computation completed with an exception.
///
/// methods in \c future allow querying the state and, most importantly,
/// scheduling a \c continuation to be executed when the future becomes
/// available.  Only one such continuation may be scheduled.
///
/// A \ref future should not be discarded before it is waited upon and
/// its result is extracted. Discarding a \ref future means that the
/// computed value becomes inaccessible, but more importantly, any
/// exceptions raised from the computation will disappear unchecked as
/// well. Another very important consequence is potentially unbounded
/// resource consumption due to the launcher of the deserted
/// continuation not being able track the amount of in-progress
/// continuations, nor their individual resource consumption.
/// To prevent accidental discarding of futures, \ref future is
/// declared `[[nodiscard]]` if the compiler supports it. Also, when a
/// discarded \ref future resolves with an error a warning is logged
/// (at runtime).
/// That said there can be legitimate cases where a \ref future is
/// discarded. The most prominent example is launching a new
/// [fiber](\ref fiber-module), or in other words, moving a continuation
/// chain to the background (off the current [fiber](\ref fiber-module)).
/// Even if a \ref future is discarded purposefully, it is still strongly
/// advisable to wait on it indirectly (via a \ref gate or
/// \ref semaphore), control their concurrency, their resource consumption
/// and handle any errors raised from them.
///
/// \tparam T A type to be carried as the result of the future, or void
///           for no result. An available future<void> only
///           contains a success/failure indication (and in the case of a
///           failure, an exception).
SEASTAR_MODULE_EXPORT
template <typename T>
class [[nodiscard]] future : private internal::future_base {
    using future_state = seastar::future_state<internal::future_stored_type_t<T>>;
    future_state _state;
    static constexpr bool copy_noexcept = future_state::copy_noexcept;

private:
    // This constructor creates a future that is not ready but has no
    // associated promise yet. The use case is to have a less flexible
    // but more efficient future/promise pair where we know that
    // promise::set_value cannot possibly be called without a matching
    // future and so that promise doesn't need to store a
    // future_state.
    future(future_for_get_promise_marker) noexcept { }

    future(promise<T>* pr) noexcept : future_base(pr, &_state), _state(std::move(pr->_local_state)) { }
    template <typename... A>
    future(ready_future_marker m, A&&... a) noexcept : _state(m, std::forward<A>(a)...) { }
    future(future_state_base::current_exception_future_marker m) noexcept : _state(m) {}
    future(future_state_base::nested_exception_marker m, future_state_base&& old) noexcept : _state(m, std::move(old)) {}
    future(future_state_base::nested_exception_marker m, future_state_base&& n, future_state_base&& old) noexcept : _state(m, std::move(n), std::move(old)) {}
    future(exception_future_marker m, std::exception_ptr&& ex) noexcept : _state(m, std::move(ex)) { }
    future(exception_future_marker m, future_state_base&& state) noexcept : _state(m, std::move(state)) { }
    [[gnu::always_inline]]
    explicit future(future_state&& state) noexcept
            : _state(std::move(state)) {
    }
    internal::promise_base_with_type<T> get_promise() noexcept {
        SEASTAR_ASSERT(!_promise);
        return internal::promise_base_with_type<T>(this);
    }
    internal::promise_base_with_type<T>* detach_promise() noexcept {
        return static_cast<internal::promise_base_with_type<T>*>(future_base::detach_promise());
    }
    void schedule(continuation_base<T>* tws) noexcept {
        future_base::schedule(tws, &tws->_state);
    }
    template <typename Pr, typename Func, typename Wrapper>
    void schedule(Pr&& pr, Func&& func, Wrapper&& wrapper) noexcept {
        // If this new throws a std::bad_alloc there is nothing that
        // can be done about it. The corresponding future is not ready
        // and we cannot break the chain. Since this function is
        // noexcept, it will call std::terminate if new throws.
        memory::scoped_critical_alloc_section _;
        auto tws = new continuation<Pr, Func, Wrapper, T>(std::move(pr), std::move(func), std::move(wrapper));
        // In a debug build we schedule ready futures, but not in
        // other build modes.
#ifdef SEASTAR_DEBUG
        if (_state.available()) {
            tws->set_state(get_available_state_ref());
            ::seastar::schedule(tws);
            return;
        }
#endif
        schedule(tws);
        _state._u.st = future_state_base::state::invalid;
    }

    [[gnu::always_inline]]
    future_state&& get_available_state_ref() noexcept {
        if (_promise) {
            detach_promise();
        }
        return std::move(_state);
    }

    future<T> rethrow_with_nested(future_state_base&& n) noexcept {
        return future<T>(future_state_base::nested_exception_marker(), std::move(n), std::move(_state));
    }

    future<T> rethrow_with_nested() noexcept {
        return future<T>(future_state_base::nested_exception_marker(), std::move(_state));
    }

    template<typename... U>
    friend class shared_future;
public:
    /// \brief The data type carried by the future.
    using value_type = internal::future_stored_type_t<T>;
    using tuple_type = internal::future_tuple_type_t<value_type>;
    /// \brief The data type carried by the future.
    using promise_type = promise<T>;
    /// \brief Moves the future into a new object.
    [[gnu::always_inline]]
    future(future&& x) noexcept : future_base(std::move(x), &_state), _state(std::move(x._state)) { }
    future(const future&) = delete;
    future& operator=(future&& x) noexcept {
        clear();
        move_it(std::move(x), &_state);
        _state = std::move(x._state);
        return *this;
    }
    void operator=(const future&) = delete;
    /// \brief gets the value returned by the computation
    ///
    /// Requires that the future be available.  If the value
    /// was computed successfully, it is returned (as an
    /// \c std::tuple).  Otherwise, an exception is thrown.
    ///
    /// If get() is called in a \ref seastar::thread context,
    /// then it need not be available; instead, the thread will
    /// be paused until the future becomes available.
    [[gnu::always_inline]]
    value_type&& get() {
        wait();
        return get_available_state_ref().take();
    }

    [[gnu::always_inline]]
    std::exception_ptr get_exception() noexcept {
        return get_available_state_ref().get_exception();
    }

    using get0_return_type = typename future_state::get0_return_type;

    /// Wait for the future to be available (in a seastar::thread)
    ///
    /// When called from a seastar::thread, this function blocks the
    /// thread until the future is available. Other threads and
    /// continuations continue to execute; only the thread is blocked.
    void wait() noexcept {
        if (_state.available()) {
            return;
        }
        do_wait();
    }

    /// \brief Checks whether the future is available.
    ///
    /// \return \c true if the future has a value, or has failed.
    [[gnu::always_inline]]
    bool available() const noexcept {
        return _state.available();
    }

    /// \brief Checks whether the future has failed.
    ///
    /// \return \c true if the future is available and has failed.
    [[gnu::always_inline]]
    bool failed() const noexcept {
        return _state.failed();
    }

    /// \brief Schedule a block of code to run when the future is ready.
    ///
    /// Schedules a function (often a lambda) to run when the future becomes
    /// available.  The function is called with the result of this future's
    /// computation as parameters.  The return value of the function becomes
    /// the return value of then(), itself as a future; this allows then()
    /// calls to be chained.
    ///
    /// If the future failed, the function is not called, and the exception
    /// is propagated into the return value of then().
    ///
    /// \param func - function to be called when the future becomes available,
    ///               unless it has failed.
    /// \return a \c future representing the return value of \c func, applied
    ///         to the eventual value of this future.
    template <typename Func, typename Result = typename internal::future_result<Func, T>::future_type>
    requires std::invocable<Func, T>
                 || (std::same_as<void, T> && std::invocable<Func>)
    Result
    then(Func&& func) noexcept {
#ifndef SEASTAR_TYPE_ERASE_MORE
        return then_impl(std::move(func));
#else
        using func_type = typename internal::future_result<Func, T>::func_type;
        noncopyable_function<func_type> ncf;
        {
            memory::scoped_critical_alloc_section _;
            ncf = noncopyable_function<func_type>([func = std::forward<Func>(func)](auto&&... args) mutable {
                return futurize_invoke(func, std::forward<decltype(args)>(args)...);
            });
        }
        return then_impl(std::move(ncf));
#endif
    }

    /// \brief Schedule a block of code to run when the future is ready, unpacking tuples.
    ///
    /// Schedules a function (often a lambda) to run when the future becomes
    /// available.  The function is called with the result of this future's
    /// computation as parameters.  The return value of the function becomes
    /// the return value of then(), itself as a future; this allows then()
    /// calls to be chained.
    ///
    /// This member function is only available when the payload is std::tuple;
    /// The tuple elements are passed as individual arguments to `func`, which
    /// must have the same arity as the tuple.
    ///
    /// If the future failed, the function is not called, and the exception
    /// is propagated into the return value of then().
    ///
    /// \param func - function to be called when the future becomes available,
    ///               unless it has failed.
    /// \return a \c future representing the return value of \c func, applied
    ///         to the eventual value of this future.
    template <typename Func, typename Result = futurize_t<internal::result_of_apply_t<Func, T>>>
    requires ::seastar::CanApplyTuple<Func, T>
    Result
    then_unpack(Func&& func) noexcept {
        return then([func = std::forward<Func>(func)] (T&& tuple) mutable {
            // sizeof...(tuple) is required to be 1
            return std::apply(func, std::move(tuple));
        });
    }

private:

    // Keep this simple so that Named Return Value Optimization is used.
    template <typename Func, typename Result>
    Result then_impl_nrvo(Func&& func) noexcept {
        using futurator = futurize<internal::future_result_t<Func, T>>;
        typename futurator::type fut(future_for_get_promise_marker{});
        using pr_type = decltype(fut.get_promise());
        schedule(fut.get_promise(), std::move(func), [](pr_type&& pr, Func& func, future_state&& state) {
            if (state.failed()) {
                pr.set_exception(static_cast<future_state_base&&>(std::move(state)));
            } else {
                futurator::satisfy_with_result_of(std::move(pr), [&func, &state] {
                    // clang thinks that "state" is not used, below, for future<>.
                    // Make it think it is used to avoid an unused-lambda-capture warning.
                    (void)state;
                    return internal::future_invoke(func, std::move(state).get_value());
                });
            }
        });
        return fut;
    }

    template <typename Func, typename Result = futurize_t<internal::future_result_t<Func, T>>>
    Result
    then_impl(Func&& func) noexcept {
#ifndef SEASTAR_DEBUG
        using futurator = futurize<internal::future_result_t<Func, T>>;
        if (failed()) {
            return futurator::make_exception_future(static_cast<future_state_base&&>(get_available_state_ref()));
        } else if (available()) {
            return futurator::invoke(std::forward<Func>(func), get_available_state_ref().take_value());
        }
#endif
        return then_impl_nrvo<Func, Result>(std::forward<Func>(func));
    }

public:
    /// \brief Schedule a block of code to run when the future is ready, allowing
    ///        for exception handling.
    ///
    /// Schedules a function (often a lambda) to run when the future becomes
    /// available.  The function is called with the this future as a parameter;
    /// it will be in an available state.  The return value of the function becomes
    /// the return value of then_wrapped(), itself as a future; this allows
    /// then_wrapped() calls to be chained.
    ///
    /// Unlike then(), the function will be called for both value and exceptional
    /// futures.
    ///
    /// \param func - function to be called when the future becomes available,
    /// \return a \c future representing the return value of \c func, applied
    ///         to the eventual value of this future.
    template <std::invocable<future> Func, typename FuncResult = std::invoke_result_t<Func, future>>
    futurize_t<FuncResult>
    then_wrapped(Func&& func) & noexcept {
        return then_wrapped_maybe_erase<false, FuncResult>(std::forward<Func>(func));
    }

    template <std::invocable<future&&> Func, typename FuncResult = std::invoke_result_t<Func, future&&>>
    futurize_t<FuncResult>
    then_wrapped(Func&& func) && noexcept {
        return then_wrapped_maybe_erase<true, FuncResult>(std::forward<Func>(func));
    }

private:

    template <bool AsSelf, typename FuncResult, typename Func>
    futurize_t<FuncResult>
    then_wrapped_maybe_erase(Func&& func) noexcept {
#ifndef SEASTAR_TYPE_ERASE_MORE
        return then_wrapped_common<AsSelf, FuncResult>(std::forward<Func>(func));
#else
        using futurator = futurize<FuncResult>;
        using WrapFuncResult = typename futurator::type;
        noncopyable_function<WrapFuncResult (future&&)> ncf;
        {
            memory::scoped_critical_alloc_section _;
            ncf = noncopyable_function<WrapFuncResult(future &&)>([func = std::forward<Func>(func)](future&& f) mutable {
                return futurator::invoke(func, std::move(f));
            });
        }
        return then_wrapped_common<AsSelf, WrapFuncResult>(std::move(ncf));
#endif
    }

    // Keep this simple so that Named Return Value Optimization is used.
    template <typename FuncResult, typename Func>
    futurize_t<FuncResult>
    then_wrapped_nrvo(Func&& func) noexcept {
        using futurator = futurize<FuncResult>;
        typename futurator::type fut(future_for_get_promise_marker{});
        using pr_type = decltype(fut.get_promise());
        schedule(fut.get_promise(), std::move(func), [](pr_type&& pr, Func& func, future_state&& state) {
            futurator::satisfy_with_result_of(std::move(pr), [&func, &state] {
                return func(future(std::move(state)));
            });
        });
        return fut;
    }


    template <bool AsSelf, typename FuncResult, typename Func>
    futurize_t<FuncResult>
    then_wrapped_common(Func&& func) noexcept {
#ifndef SEASTAR_DEBUG
        using futurator = futurize<FuncResult>;
        if (available()) {
            if constexpr (AsSelf) {
                if (_promise) {
                    detach_promise();
                }
                return futurator::invoke(std::forward<Func>(func), std::move(*this));
            } else {
                return futurator::invoke(std::forward<Func>(func), future(get_available_state_ref()));
            }
        }
#endif
        return then_wrapped_nrvo<FuncResult, Func>(std::forward<Func>(func));
    }

    void forward_to(internal::promise_base_with_type<T>&& pr) noexcept {
        if (_state.available()) {
            pr.set_urgent_state(std::move(_state));
        } else {
            *detach_promise() = std::move(pr);
        }
    }

public:
    /// \brief Satisfy some \ref promise object with this future as a result.
    ///
    /// Arranges so that when this future is resolve, it will be used to
    /// satisfy an unrelated promise.  This is similar to scheduling a
    /// continuation that moves the result of this future into the promise
    /// (using promise::set_value() or promise::set_exception(), except
    /// that it is more efficient.
    ///
    /// \param pr a promise that will be fulfilled with the results of this
    /// future.
    void forward_to(promise<T>&& pr) noexcept {
        if (_state.available()) {
            pr.set_urgent_state(std::move(_state));
        } else if (&pr._local_state != pr._state) {
            // The only case when _state points to _local_state is
            // when get_future was never called. Given that pr will
            // soon be destroyed, we know get_future will never be
            // called and we can just ignore this request.
            *detach_promise() = std::move(pr);
        }
    }



    /**
     * Finally continuation for statements that require waiting for the result.
     * I.e. you need to "finally" call a function that returns a possibly
     * unavailable future. The returned future will be "waited for", any
     * exception generated will be propagated, but the return value is ignored.
     * I.e. the original return value (the future upon which you are making this
     * call) will be preserved.
     *
     * If the original return value or the callback return value is an
     * exceptional future it will be propagated.
     *
     * If both of them are exceptional - the seastar::nested_exception exception
     * with the callback exception on top and the original future exception
     * nested will be propagated.
     */
    template <std::invocable Func>
    future<T> finally(Func&& func) noexcept {
        return then_wrapped(finally_body<Func, is_future<std::invoke_result_t<Func>>::value>(std::forward<Func>(func)));
    }


    template <typename Func, bool FuncReturnsFuture>
    struct finally_body;

    template <typename Func>
    struct finally_body<Func, true> {
        Func _func;

        finally_body(Func&& func) noexcept : _func(std::forward<Func>(func))
        { }

        future<T> operator()(future<T>&& result) noexcept {
            return futurize_invoke(_func).then_wrapped([result = std::move(result)](auto&& f_res) mutable {
                if (!f_res.failed()) {
                    return std::move(result);
                } else {
                    return result.rethrow_with_nested(std::move(f_res._state));
                }
            });
        }
    };

    template <typename Func>
    struct finally_body<Func, false> {
        Func _func;

        finally_body(Func&& func) noexcept : _func(std::forward<Func>(func))
        { }

        future<T> operator()(future<T>&& result) noexcept {
            try {
                _func();
                return std::move(result);
            } catch (...) {
                return result.rethrow_with_nested();
            }
        };
    };

    /// \brief Terminate the program if this future fails.
    ///
    /// Terminates the entire program is this future resolves
    /// to an exception.  Use with caution.
    future<> or_terminate() noexcept {
        return then_wrapped([] (auto&& f) {
            try {
                f.get();
            } catch (...) {
                engine_exit(std::current_exception());
            }
        });
    }

    /// \brief Discards the value carried by this future.
    ///
    /// Converts the future into a no-value \c future<>, by
    /// ignoring any result.  Exceptions are propagated unchanged.
    future<> discard_result() noexcept {
        // We need the generic variadic lambda, below, because then() behaves differently
        // when value_type is when_all_succeed_tuple
        return then([] (auto&&...) {});
    }

    /// \brief Handle the exception carried by this future.
    ///
    /// When the future resolves, if it resolves with an exception,
    /// handle_exception(func) replaces the exception with the value
    /// returned by func. The exception is passed (as a std::exception_ptr)
    /// as a parameter to func; func may return the replacement value
    /// immediately (T or std::tuple<T...>) or in the future (future<T...>)
    /// and is even allowed to return (or throw) its own exception.
    ///
    /// The idiom fut.discard_result().handle_exception(...) can be used
    /// to handle an exception (if there is one) without caring about the
    /// successful value; Because handle_exception() is used here on a
    /// future<>, the handler function does not need to return anything.
    template <typename Func>
    requires std::is_invocable_r_v<future<T> ,Func, std::exception_ptr>
                    || (std::tuple_size_v<tuple_type> == 0 && std::is_invocable_r_v<void, Func, std::exception_ptr>)
                    || (std::tuple_size_v<tuple_type> == 1 && std::is_invocable_r_v<T, Func, std::exception_ptr>)
                    || (std::tuple_size_v<tuple_type> > 1 && std::is_invocable_r_v<tuple_type ,Func, std::exception_ptr>)
    future<T> handle_exception(Func&& func) noexcept {
        return then_wrapped([func = std::forward<Func>(func)]
                             (auto&& fut) mutable -> future<T> {
            if (!fut.failed()) {
                return make_ready_future<T>(fut.get());
            } else {
                return futurize_invoke(func, fut.get_exception());
            }
        });
    }

    /// \brief Handle the exception of a certain type carried by this future.
    ///
    /// When the future resolves, if it resolves with an exception of a type that
    /// provided callback receives as a parameter, \c handle_exception_type(func) replaces
    /// the exception with the value returned by func. The exception is passed (by
    /// reference) as a parameter to func; func may return the replacement value
    /// immediately (T or std::tuple<T...>) or in the future (future<T...>)
    /// and is even allowed to return (or throw) its own exception.
    /// If exception, that future holds, does not match func parameter type
    /// it is propagated as is.
    template <typename Func>
    future<T> handle_exception_type(Func&& func) noexcept {
        using trait = function_traits<Func>;
        static_assert(trait::arity == 1, "func can take only one parameter");
        using ex_type = typename trait::template arg<0>::type;
        return then_wrapped([func = std::forward<Func>(func)]
                             (auto&& fut) mutable -> future<T> {
            try {
                return make_ready_future<T>(fut.get());
            } catch(ex_type& ex) {
                return futurize_invoke(func, ex);
            }
        });
    }

    /// \brief Ignore any result hold by this future
    ///
    /// Ignore any result (value or exception) hold by this future.
    /// Use with caution since usually ignoring exception is not what
    /// you want
    void ignore_ready_future() noexcept {
        _state.ignore();
    }

    using future_base::set_coroutine;

private:
    void set_task(task& t) noexcept {
        SEASTAR_ASSERT(_promise);
        _promise->set_task(&t);
    }

    void set_callback(continuation_base<T>* callback) noexcept {
        if (_state.available()) {
            callback->set_state(get_available_state_ref());
            ::seastar::schedule(callback);
        } else {
            SEASTAR_ASSERT(_promise);
            schedule(callback);
        }

    }

    /// \cond internal
    template <typename U>
    friend class future;
    template <typename U>
    friend class promise;
    template <typename U>
    friend struct futurize;
    template <typename U>
    friend class internal::promise_base_with_type;
    template <typename U, typename... A>
    friend future<U> make_ready_future(A&&... value) noexcept;
    template <typename U>
    friend future<U> make_exception_future(std::exception_ptr&& ex) noexcept;
    template <typename U, typename Exception>
    friend future<U> make_exception_future(Exception&& ex) noexcept;
    template <typename U>
    friend future<U> internal::make_exception_future(future_state_base&& state) noexcept;
    template <typename U>
    friend future<U> current_exception_as_future() noexcept;
    template <typename U>
    friend void internal::set_callback(future<U>&&, continuation_base<U>*) noexcept;
    /// \endcond
};


namespace internal {
template <typename T>
struct futurize_base {
    /// If \c T is a future, \c T; otherwise \c future<T>
    using type = future<T>;
    /// The promise type associated with \c type.
    using promise_type = promise<T>;
    using promise_base_with_type = internal::promise_base_with_type<T>;

    /// Convert a value or a future to a future
    static inline type convert(T&& value) { return make_ready_future<T>(std::move(value)); }
    static inline type convert(type&& value) { return std::move(value); }

    /// Makes an exceptional future of type \ref type.
    template <typename Arg>
    static inline type make_exception_future(Arg&& arg) noexcept;
};

template <>
struct futurize_base<void> {
    using type = future<>;
    using promise_type = promise<>;
    using promise_base_with_type = internal::promise_base_with_type<>;

    static inline type convert(type&& value) {
        return std::move(value);
    }
    template <typename Arg>
    static inline type make_exception_future(Arg&& arg) noexcept;
};

template <typename T>
struct futurize_base<future<T>> : public futurize_base<T> {};

template <>
struct futurize_base<future<>> : public futurize_base<void> {};
}

template <typename T>
struct futurize : public internal::futurize_base<T> {
    using base = internal::futurize_base<T>;
    using type = typename base::type;
    using promise_type = typename base::promise_type;
    using promise_base_with_type = typename base::promise_base_with_type;
    /// The value tuple type associated with \c type
    using value_type = typename type::value_type;
    using tuple_type = typename type::tuple_type;
    using base::convert;
    using base::make_exception_future;

    /// Apply a function to an argument list (expressed as a tuple)
    /// and return the result, as a future (if it wasn't already).
    template<typename Func, typename... FuncArgs>
    static inline type apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept;

    /// Invoke a function to an argument list
    /// and return the result, as a future (if it wasn't already).
    template<typename Func, typename... FuncArgs>
    static inline type invoke(Func&& func, FuncArgs&&... args) noexcept;

    template<typename Func>
    static inline type invoke(Func&& func, internal::monostate) noexcept {
        return invoke(std::forward<Func>(func));
    }

    static type current_exception_as_future() noexcept {
        return type(future_state_base::current_exception_future_marker());
    }

    /// Convert the tuple representation into a future
    static type from_tuple(tuple_type&& value) {
        return type(ready_future_marker(), std::move(value));
    }
    /// Convert the tuple representation into a future
    static type from_tuple(const tuple_type& value) {
        return type(ready_future_marker(), value);
    }

    /// Convert the tuple representation into a future
    static type from_tuple(value_type&& value) {
        return type(ready_future_marker(), std::move(value));
    }
    /// Convert the tuple representation into a future
    static type from_tuple(const value_type& value) {
        return type(ready_future_marker(), value);
    }
private:
    /// Forwards the result of, or exception thrown by, func() to the
    /// promise. This avoids creating a future if func() doesn't
    /// return one.
    template<std::invocable Func>
    static void satisfy_with_result_of(promise_base_with_type&&, Func&& func);

    template <typename U>
    friend class future;
};

inline internal::promise_base::promise_base(future_base* future, future_state_base* state) noexcept
    : _future(future), _state(state) {
    _future->_promise = this;
}

template <typename T>
inline
future<T>
promise<T>::get_future() noexcept {
    SEASTAR_ASSERT(!this->_future && this->_state && !this->_task);
    return future<T>(this);
}

template <typename T>
inline
void promise<T>::move_it(promise&& x) noexcept {
    if (this->_state == &x._local_state) {
        this->_state = &_local_state;
        new (&_local_state) future_state(std::move(x._local_state));
    }
}

SEASTAR_MODULE_EXPORT_BEGIN
template <typename T, typename... A>
inline
future<T> make_ready_future(A&&... value) noexcept {
    return future<T>(ready_future_marker(), std::forward<A>(value)...);
}

template <typename T>
inline
future<T> make_exception_future(std::exception_ptr&& ex) noexcept {
    return future<T>(exception_future_marker(), std::move(ex));
}
SEASTAR_MODULE_EXPORT_END

template <typename T>
inline
future<T> internal::make_exception_future(future_state_base&& state) noexcept {
    return future<T>(exception_future_marker(), std::move(state));
}

SEASTAR_MODULE_EXPORT_BEGIN
template <typename T>
future<T> current_exception_as_future() noexcept {
    return future<T>(future_state_base::current_exception_future_marker());
}

void log_exception_trace() noexcept;

/// \brief Creates a \ref future in an available, failed state.
///
/// Creates a \ref future object that is already resolved in a failed
/// state.  This no I/O needs to be performed to perform a computation
/// (for example, because the connection is closed and we cannot read
/// from it).
template <typename T, typename Exception>
inline
future<T> make_exception_future(Exception&& ex) noexcept {
    log_exception_trace();
    return make_exception_future<T>(std::make_exception_ptr(std::forward<Exception>(ex)));
}

template <typename T, typename Exception>
future<T> make_exception_future_with_backtrace(Exception&& ex) noexcept {
    return make_exception_future<T>(make_backtraced_exception_ptr<Exception>(std::forward<Exception>(ex)));
}
SEASTAR_MODULE_EXPORT_END

/// @}

/// \cond internal

template<typename T>
template<typename Func, typename... FuncArgs>
typename futurize<T>::type futurize<T>::apply(Func&& func, std::tuple<FuncArgs...>&& args) noexcept {
    try {
        using ret_t = decltype(std::apply(std::forward<Func>(func), std::move(args)));
        if constexpr (std::is_void_v<ret_t>) {
            std::apply(std::forward<Func>(func), std::move(args));
            return make_ready_future<>();
        } else if constexpr (is_future<ret_t>::value){
            return std::apply(std::forward<Func>(func), std::move(args));
        } else {
            return convert(std::apply(std::forward<Func>(func), std::move(args)));
        }
    } catch (...) {
        return current_exception_as_future();
    }
}

template<typename T>
template<std::invocable Func>
void futurize<T>::satisfy_with_result_of(promise_base_with_type&& pr, Func&& func) {
    using ret_t = decltype(func());
    if constexpr (std::is_void_v<ret_t>) {
        func();
        pr.set_value();
    } else if constexpr (is_future<ret_t>::value) {
        func().forward_to(std::move(pr));
    } else {
        pr.set_value(func());
    }
}

template<typename T>
template<typename Func, typename... FuncArgs>
typename futurize<T>::type futurize<T>::invoke(Func&& func, FuncArgs&&... args) noexcept {
    try {
        using ret_t = decltype(func(std::forward<FuncArgs>(args)...));
        if constexpr (std::is_void_v<ret_t>) {
            func(std::forward<FuncArgs>(args)...);
            return make_ready_future<>();
        } else if constexpr (is_future<ret_t>::value) {
            return func(std::forward<FuncArgs>(args)...);
        } else {
            return convert(func(std::forward<FuncArgs>(args)...));
        }
    } catch (...) {
        return current_exception_as_future();
    }
}

template <typename T>
template <typename Arg>
inline
future<T>
internal::futurize_base<T>::make_exception_future(Arg&& arg) noexcept {
    using ::seastar::make_exception_future;
    using ::seastar::internal::make_exception_future;
    return make_exception_future<T>(std::forward<Arg>(arg));
}

template <typename Arg>
inline
future<>
internal::futurize_base<void>::make_exception_future(Arg&& arg) noexcept {
    using ::seastar::make_exception_future;
    using ::seastar::internal::make_exception_future;
    return make_exception_future<>(std::forward<Arg>(arg));
}

template<typename Func, typename... Args>
auto futurize_invoke(Func&& func, Args&&... args) noexcept {
    using futurator = futurize<std::invoke_result_t<Func, Args&&...>>;
    return futurator::invoke(std::forward<Func>(func), std::forward<Args>(args)...);
}

template<typename Func, typename... Args>
auto futurize_apply(Func&& func, std::tuple<Args...>&& args) noexcept {
    using futurator = futurize<std::invoke_result_t<Func, Args&&...>>;
    return futurator::apply(std::forward<Func>(func), std::move(args));
}

namespace internal {

template <typename T>
inline
void set_callback(future<T>&& fut, continuation_base<T>* callback) noexcept {
    return std::move(fut).set_callback(callback);
}

}


/// \endcond

}
