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

#include <cassert>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <seastar/core/task.hh>
#include <seastar/core/thread_impl.hh>
#include <seastar/core/function_traits.hh>
#include <seastar/util/critical_alloc_section.hh>
#include <seastar/util/concepts.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/util/std-compat.hh>

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

#if SEASTAR_API_LEVEL < 6
template <class... T>
#else
template <class T = void>
#endif
class promise;

template <class SEASTAR_ELLIPSIS T>
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
template <typename... T, typename... A>
future<T...> make_ready_future(A&&... value) noexcept;

/// \brief Creates a \ref future in an available, failed state.
///
/// Creates a \ref future object that is already resolved in a failed
/// state.  This is useful when no I/O needs to be performed to perform
/// a computation (for example, because the connection is closed and
/// we cannot read from it).
template <typename... T>
future<T...> make_exception_future(std::exception_ptr&& value) noexcept;

template <typename... T>
future<T...> make_exception_future(const std::exception_ptr& ex) noexcept {
    return make_exception_future<T...>(std::exception_ptr(ex));
}

template <typename... T>
future<T...> make_exception_future(std::exception_ptr& ex) noexcept {
    return make_exception_future<T...>(static_cast<const std::exception_ptr&>(ex));
}

template <typename... T>
future<T...> make_exception_future(const std::exception_ptr&& ex) noexcept {
    // as ex is const, we cannot move it, but can copy it.
    return make_exception_future<T...>(std::exception_ptr(ex));
}

/// \cond internal
void engine_exit(std::exception_ptr eptr = {});

void report_failed_future(const std::exception_ptr& ex) noexcept;

void report_failed_future(const future_state_base& state) noexcept;

void with_allow_abandoned_failed_futures(unsigned count, noncopyable_function<void ()> func);

/// \endcond

/// \brief Exception type for broken promises
///
/// When a promise is broken, i.e. a promise object with an attached
/// continuation is destroyed before setting any value or exception, an
/// exception of `broken_promise` type is propagated to that abandoned
/// continuation.
struct broken_promise : std::logic_error {
    broken_promise();
};

/// \brief Returns std::current_exception() wrapped in a future
///
/// This is equivalent to
/// make_exception_future(std::current_exception()), but expands to
/// less code.
template <typename... T>
future<T...> current_exception_as_future() noexcept;

extern template
future<> current_exception_as_future() noexcept;

namespace internal {
#if SEASTAR_API_LEVEL < 6
template <class... T>
#else
template <class T = void>
#endif
class promise_base_with_type;
class promise_base;

struct monostate {};

template <typename... T>
struct future_stored_type;

template <>
struct future_stored_type<> {
#if SEASTAR_API_LEVEL < 5
    using type = std::tuple<>;
#else
    using type = monostate;
#endif
};

template <typename T>
struct future_stored_type<T> {
#if SEASTAR_API_LEVEL < 5
    using type = std::tuple<T>;
#else
    using type = std::conditional_t<std::is_void_v<T>, internal::monostate, T>;
#endif
};

template <typename... T>
using future_stored_type_t = typename future_stored_type<T...>::type;

template<typename T>
#if SEASTAR_API_LEVEL < 5
using future_tuple_type_t = T;
#else
using future_tuple_type_t = std::conditional_t<std::is_same_v<T, monostate>, std::tuple<>, std::tuple<T>>;
#endif

// It doesn't seem to be possible to use std::tuple_element_t with an empty tuple. There is an static_assert in it that
// fails the build even if it is in the non enabled side of std::conditional.
template <typename T>
struct get0_return_type;

template <>
struct get0_return_type<std::tuple<>> {
    using type = void;
    static type get0(std::tuple<>) { }
};

template <typename T0, typename... T>
struct get0_return_type<std::tuple<T0, T...>> {
    using type = T0;
    static type get0(std::tuple<T0, T...> v) { return std::get<0>(std::move(v)); }
};

template<typename T>
using maybe_wrap_ref = std::conditional_t<std::is_reference_v<T>, std::reference_wrapper<std::remove_reference_t<T>>, T>;

/// \brief Wrapper for keeping uninitialized values of non default constructible types.
///
/// This is similar to a std::optional<T>, but it doesn't know if it is holding a value or not, so the user is
/// responsible for calling constructors and destructors.
///
/// The advantage over just using a union directly is that this uses inheritance when possible and so benefits from the
/// empty base optimization.
template <typename T, bool is_trivial_class>
struct uninitialized_wrapper_base;

template <typename T>
struct uninitialized_wrapper_base<T, false> {
    using tuple_type = future_tuple_type_t<T>;
    union any {
        any() noexcept {}
        ~any() {}
        // T can be a reference, so wrap it.
        maybe_wrap_ref<T> value;
    } _v;

public:
    uninitialized_wrapper_base() noexcept = default;
    template<typename... U>
    std::enable_if_t<!std::is_same_v<std::tuple<std::remove_cv_t<U>...>, std::tuple<tuple_type>>, void>
    uninitialized_set(U&&... vs) {
        new (&_v.value) maybe_wrap_ref<T>{T(std::forward<U>(vs)...)};
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

template <typename T> struct uninitialized_wrapper_base<T, true> : private T {
    using tuple_type = future_tuple_type_t<T>;
    uninitialized_wrapper_base() noexcept = default;
    template<typename... U>
    std::enable_if_t<!std::is_same_v<std::tuple<std::remove_cv_t<U>...>, std::tuple<tuple_type>>, void>
    uninitialized_set(U&&... vs) {
        new (this) T(std::forward<U>(vs)...);
    }
    void uninitialized_set(tuple_type&& v) {
        if constexpr (std::tuple_size_v<tuple_type> != 0) {
            uninitialized_set(std::move(std::get<0>(v)));
        }
    }
    void uninitialized_set(const tuple_type& v) {
        if constexpr (std::tuple_size_v<tuple_type> != 0) {
            uninitialized_set(std::get<0>(v));
        }
    }
    T& uninitialized_get() {
        return *this;
    }
    const T& uninitialized_get() const {
        return *this;
    }
};

template <typename T>
constexpr bool can_inherit =
#ifdef _LIBCPP_VERSION
// We expect std::tuple<> to be trivially constructible and
// destructible. That is not the case with libc++
// (https://bugs.llvm.org/show_bug.cgi?id=41714).  We could avoid this
// optimization when using libc++ and relax the asserts, but
// inspection suggests that std::tuple<> is trivial, it is just not
// marked as such.
        std::is_same<std::tuple<>, T>::value ||
#endif
        (std::is_trivially_destructible<T>::value && std::is_trivially_constructible<T>::value &&
                std::is_class<T>::value && !std::is_final<T>::value);

// The objective is to avoid extra space for empty types like std::tuple<>. We could use std::is_empty_v, but it is
// better to check that both the constructor and destructor can be skipped.
template <typename T>
struct uninitialized_wrapper
    : public uninitialized_wrapper_base<T, can_inherit<T>> {};

template <typename T>
struct is_trivially_move_constructible_and_destructible {
    static constexpr bool value = std::is_trivially_move_constructible<T>::value && std::is_trivially_destructible<T>::value;
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
            assert(st >= state::exception_min);
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
        assert(_u.st == state::future);
        _u.set_exception(std::move(ex));
    }
    future_state_base& operator=(future_state_base&& x) noexcept = default;
    void set_exception(future_state_base&& state) noexcept {
        assert(_u.st == state::future);
        *this = std::move(state);
    }
    std::exception_ptr get_exception() && noexcept {
        assert(_u.st >= state::exception_min);
        // Move ex out so future::~future() knows we've handled it
        return _u.take_exception();
    }
    const std::exception_ptr& get_exception() const& noexcept {
        assert(_u.st >= state::exception_min);
        return _u.ex;
    }
    template <typename U>
    friend struct future_state;
    template <typename... U>
    friend future<U...> current_exception_as_future() noexcept;
    template <typename SEASTAR_ELLIPSIS U>
    friend class future;
    template <typename T>
    friend struct futurize;
};

void report_failed_future(future_state_base::any&& state) noexcept;

inline void future_state_base::any::check_failure() noexcept {
    if (failed()) {
        report_failed_future(std::move(*this));
    }
}

struct ready_future_marker {};
struct exception_future_marker {};
struct future_for_get_promise_marker {};

/// \cond internal
template <typename T>
struct future_state :  public future_state_base, private internal::uninitialized_wrapper<T> {
    static constexpr bool copy_noexcept = std::is_nothrow_copy_constructible<T>::value;
#if SEASTAR_API_LEVEL < 5
    static constexpr bool has_trivial_move_and_destroy = internal::is_tuple_effectively_trivially_move_constructible_and_destructible<T>;
#else
    static constexpr bool has_trivial_move_and_destroy = internal::is_trivially_move_constructible_and_destructible<T>::value;
#endif
    static_assert(std::is_nothrow_move_constructible<T>::value,
                  "Types must be no-throw move constructible");
    static_assert(std::is_nothrow_destructible<T>::value,
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
        assert(_u.st == state::future);
        new (this) future_state(ready_future_marker(), std::forward<A>(a)...);
    }
    future_state(exception_future_marker, std::exception_ptr&& ex) noexcept : future_state_base(std::move(ex)) { }
    future_state(exception_future_marker, future_state_base&& state) noexcept : future_state_base(std::move(state)) { }
    future_state(current_exception_future_marker m) noexcept : future_state_base(m) { }
    future_state(nested_exception_marker m, future_state_base&& old) noexcept : future_state_base(m, std::move(old)) { }
    future_state(nested_exception_marker m, future_state_base&& n, future_state_base&& old) noexcept : future_state_base(m, std::move(n), std::move(old)) { }
    T&& get_value() && noexcept {
        assert(_u.st == state::result);
        return static_cast<T&&>(this->uninitialized_get());
    }
    T&& take_value() && noexcept {
        assert(_u.st == state::result);
        _u.st = state::result_unavailable;
        return static_cast<T&&>(this->uninitialized_get());
    }
    template<typename U = T>
    const std::enable_if_t<std::is_copy_constructible<U>::value, U>& get_value() const& noexcept(copy_noexcept) {
        assert(_u.st == state::result);
        return this->uninitialized_get();
    }
    T&& take() && {
        assert(available());
        if (_u.st >= state::exception_min) {
            std::move(*this).rethrow_exception();
        }
        _u.st = state::result_unavailable;
        return static_cast<T&&>(this->uninitialized_get());
    }
    T&& get() && {
        assert(available());
        if (_u.st >= state::exception_min) {
            std::move(*this).rethrow_exception();
        }
        return static_cast<T&&>(this->uninitialized_get());
    }
    const T& get() const& {
        assert(available());
        if (_u.st >= state::exception_min) {
            rethrow_exception();
        }
        return this->uninitialized_get();
    }
    using get0_return_type = typename internal::get0_return_type<internal::future_tuple_type_t<T>>::type;
    static get0_return_type get0(T&& x) {
        return internal::get0_return_type<T>::get0(std::move(x));
    }

    get0_return_type get0() {
#if SEASTAR_API_LEVEL < 5
        return get0(std::move(*this).get());
#else
        return std::move(*this).get();
#endif
    }
};

#if SEASTAR_API_LEVEL < 6
template <typename... T>
#else
template <typename T = void>
#endif
class continuation_base : public task {
protected:
    using future_state = seastar::future_state<internal::future_stored_type_t<T SEASTAR_ELLIPSIS>>;
    future_state _state;
    using future_type = future<T SEASTAR_ELLIPSIS>;
    using promise_type = promise<T SEASTAR_ELLIPSIS>;
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
    friend class internal::promise_base_with_type<T SEASTAR_ELLIPSIS>;
    friend class promise<T SEASTAR_ELLIPSIS>;
    friend class future<T SEASTAR_ELLIPSIS>;
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

#if SEASTAR_API_LEVEL < 6
template <typename Promise, typename... T>
#else
template <typename Promise, typename T = void>
#endif
class continuation_base_with_promise : public continuation_base<T SEASTAR_ELLIPSIS> {
    friend class internal::promise_base_with_type<T SEASTAR_ELLIPSIS>;
protected:
    continuation_base_with_promise(Promise&& pr) noexcept : _pr(std::move(pr)) {
        task::make_backtrace();
    }
    virtual task* waiting_task() noexcept override;
    Promise _pr;
};

#if SEASTAR_API_LEVEL < 6
template <typename Promise, typename Func, typename Wrapper, typename... T>
#else
template <typename Promise, typename Func, typename Wrapper, typename T = void>
#endif
struct continuation final : continuation_base_with_promise<Promise, T SEASTAR_ELLIPSIS> {
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
        : continuation_base_with_promise<Promise, T SEASTAR_ELLIPSIS>(std::move(pr))
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

#if SEASTAR_API_LEVEL < 4

// This is an internal future<> payload for seastar::when_all_succeed(). It is used
// to return a variadic future (when two or more of its input futures were non-void),
// but with variadic futures deprecated and soon gone this is no longer possible.
//
// Instead, we use this tuple type, and future::then() knows to unpack it.
//
// The whole thing is temporary for a transition period.
template <typename... T>
struct when_all_succeed_tuple : std::tuple<T...> {
    using std::tuple<T...>::tuple;
    when_all_succeed_tuple(std::tuple<T...>&& t)
            noexcept(std::is_nothrow_move_constructible<std::tuple<T...>>::value)
            : std::tuple<T...>(std::move(t)) {}
};

#endif

namespace internal {

template <typename... T>
future<T...> make_exception_future(future_state_base&& state) noexcept;

template <typename... T, typename U>
void set_callback(future<T...>&& fut, U* callback) noexcept;

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
            report_failed_future(val);
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
    std::enable_if_t<!std::is_same<std::remove_reference_t<Exception>, std::exception_ptr>::value, void> set_exception(Exception&& e) noexcept {
        set_exception(std::make_exception_ptr(std::forward<Exception>(e)));
    }

    friend class future_base;
    template <typename SEASTAR_ELLIPSIS U> friend class seastar::future;

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
template <typename SEASTAR_ELLIPSIS T>
class promise_base_with_type : protected internal::promise_base {
protected:
    using future_state = seastar::future_state<future_stored_type_t<T SEASTAR_ELLIPSIS>>;
    future_state* get_state() noexcept {
        return static_cast<future_state*>(_state);
    }
    static constexpr bool copy_noexcept = future_state::copy_noexcept;
public:
    promise_base_with_type(future_state_base* state) noexcept : promise_base(state) { }
    promise_base_with_type(future<T SEASTAR_ELLIPSIS>* future) noexcept : promise_base(future, &future->_state) { }
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
            assert(ptr->_u.st == future_state_base::state::future);
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

    template <typename SEASTAR_ELLIPSIS U>
    friend class seastar::future;

    friend future_state;
};
}
/// \endcond

/// \brief promise - allows a future value to be made available at a later time.
///
/// \tparam T A list of types to be carried as the result of the associated future.
///           A list with two or more types is deprecated; use
///           \c promise<std::tuple<T...>> instead.
template <typename SEASTAR_ELLIPSIS T>
class promise : private internal::promise_base_with_type<T SEASTAR_ELLIPSIS> {
    using future_state = typename internal::promise_base_with_type<T SEASTAR_ELLIPSIS>::future_state;
    future_state _local_state;

public:
    /// \brief Constructs an empty \c promise.
    ///
    /// Creates promise with no associated future yet (see get_future()).
    promise() noexcept : internal::promise_base_with_type<T SEASTAR_ELLIPSIS>(&_local_state) {}

    /// \brief Moves a \c promise object.
    void move_it(promise&& x) noexcept;
    promise(promise&& x) noexcept : internal::promise_base_with_type<T SEASTAR_ELLIPSIS>(std::move(x)) {
        move_it(std::move(x));
    }
    promise(const promise&) = delete;
    promise& operator=(promise&& x) noexcept {
        internal::promise_base_with_type<T SEASTAR_ELLIPSIS>::operator=(std::move(x));
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
    future<T SEASTAR_ELLIPSIS> get_future() noexcept;

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
        internal::promise_base_with_type<T SEASTAR_ELLIPSIS>::set_value(std::forward<A>(a)...);
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
    std::enable_if_t<!std::is_same<std::remove_reference_t<Exception>, std::exception_ptr>::value, void> set_exception(Exception&& e) noexcept {
        internal::promise_base::set_exception(std::forward<Exception>(e));
    }

    using internal::promise_base_with_type<T SEASTAR_ELLIPSIS>::set_urgent_state;

    template <typename SEASTAR_ELLIPSIS U>
    friend class future;
};

#if SEASTAR_API_LEVEL < 6
/// \brief Specialization of \c promise<void>
///
/// This is an alias for \c promise<>, for generic programming purposes.
/// For example, You may have a \c promise<T> where \c T can legally be
/// \c void.
template<>
class promise<void> : public promise<> {};
#endif

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
template <typename T>
struct futurize;

SEASTAR_CONCEPT(

template <typename T>
concept Future = is_future<T>::value;

template <typename Func, typename... T>
concept CanInvoke = std::invocable<Func, T...>;

// Deprecated alias
template <typename Func, typename... T>
concept CanApply = CanInvoke<Func, T...>;

template <typename Func, typename... T>
concept CanApplyTuple
    = sizeof...(T) == 1
        && requires (Func func, std::tuple<T...> wrapped_val) {
        { std::apply(func, std::get<0>(std::move(wrapped_val))) };
    };

template <typename Func, typename Return, typename... T>
concept InvokeReturns = requires (Func f, T... args) {
    { f(std::forward<T>(args)...) } -> std::same_as<Return>;
};

// Deprecated alias
template <typename Func, typename Return, typename... T>
concept ApplyReturns = InvokeReturns<Func, Return, T...>;

template <typename Func, typename... T>
concept InvokeReturnsAnyFuture = Future<std::invoke_result_t<Func, T...>>;

// Deprecated alias
template <typename Func, typename... T>
concept ApplyReturnsAnyFuture = InvokeReturnsAnyFuture<Func, T...>;

)

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
        p->_task = tws;
    }

    void do_wait() noexcept;

#ifdef SEASTAR_COROUTINES_ENABLED
    void set_coroutine(task& coroutine) noexcept;
#endif

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

template <typename Func, typename SEASTAR_ELLIPSIS T>
using future_result_t = typename future_result<Func, T SEASTAR_ELLIPSIS>::type;

template <typename Func, typename T>
auto future_invoke(Func&& func, T&& v) {
    if constexpr (std::is_same_v<T, monostate>) {
        return std::invoke(std::forward<Func>(func));
    } else {
        return std::invoke(std::forward<Func>(func), std::forward<T>(v));
    }
}

// This is a customization point for future::then()'s implementation.
// It behaves differently when the future value type is a when_all_succeed_tuple
// instantiation, indicating we need to unpack the tuple into multiple lambda
// arguments.
template <typename Future>
struct call_then_impl;

// Generic case - the input is not a future<when_all_succeed_tuple<...>>, so
// we just forward everything to future::then_impl.
template <typename... T>
struct call_then_impl<future<T...>> {
    template <typename Func>
    using result_type = typename future_result<Func, T...>::future_type;

    template <typename Func>
    using func_type = typename future_result<Func, T...>::func_type;

    template <typename Func>
    static result_type<Func> run(future<T...>& fut, Func&& func) noexcept {
        return fut.then_impl(std::forward<Func>(func));
    }
};

#if SEASTAR_API_LEVEL < 4

// Special case: we unpack the tuple before calling the function
template <typename... T>
struct call_then_impl<future<when_all_succeed_tuple<T...>>> {
    template <typename Func>
    using result_type = futurize_t<std::invoke_result_t<Func,  T&&...>>;

    template <typename Func>
    using func_type = result_type<Func> (T&&...);

    using was_tuple = when_all_succeed_tuple<T...>;
    using std_tuple = std::tuple<T...>;

    template <typename Func>
    static auto run(future<was_tuple>& fut, Func&& func) noexcept {
        // constructing func in the lambda can throw, but there's nothing we can do
        // about it, similar to #84.
        return fut.then_impl([func = std::forward<Func>(func)] (was_tuple&& t) mutable {
            return std::apply(func, static_cast<std_tuple&&>(std::move(t)));
        });
    }
};

#endif

template <typename Func, typename... Args>
using call_then_impl_result_type = typename call_then_impl<future<Args...>>::template result_type<Func>;

SEASTAR_CONCEPT(
template <typename Func, typename... Args>
concept CanInvokeWhenAllSucceed = requires {
    typename call_then_impl_result_type<Func, Args...>;
};
)

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

template <typename Promise, typename SEASTAR_ELLIPSIS T>
task* continuation_base_with_promise<Promise, T SEASTAR_ELLIPSIS>::waiting_task() noexcept {
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
/// \tparam T A list of types to be carried as the result of the future,
///           similar to \c std::tuple<T...>. An empty list (\c future<>)
///           means that there is no result, and an available future only
///           contains a success/failure indication (and in the case of a
///           failure, an exception).
///           A list with two or more types is deprecated; use
///           \c future<std::tuple<T...>> instead.
template <typename SEASTAR_ELLIPSIS T>
class [[nodiscard]] future : private internal::future_base {
    using future_state = seastar::future_state<internal::future_stored_type_t<T SEASTAR_ELLIPSIS>>;
    future_state _state;
    static constexpr bool copy_noexcept = future_state::copy_noexcept;
    using call_then_impl = internal::call_then_impl<future>;

private:
    // This constructor creates a future that is not ready but has no
    // associated promise yet. The use case is to have a less flexible
    // but more efficient future/promise pair where we know that
    // promise::set_value cannot possibly be called without a matching
    // future and so that promise doesn't need to store a
    // future_state.
    future(future_for_get_promise_marker) noexcept { }

    future(promise<T SEASTAR_ELLIPSIS>* pr) noexcept : future_base(pr, &_state), _state(std::move(pr->_local_state)) { }
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
    internal::promise_base_with_type<T SEASTAR_ELLIPSIS> get_promise() noexcept {
        assert(!_promise);
        return internal::promise_base_with_type<T SEASTAR_ELLIPSIS>(this);
    }
    internal::promise_base_with_type<T SEASTAR_ELLIPSIS>* detach_promise() noexcept {
        return static_cast<internal::promise_base_with_type<T SEASTAR_ELLIPSIS>*>(future_base::detach_promise());
    }
    void schedule(continuation_base<T SEASTAR_ELLIPSIS>* tws) noexcept {
        future_base::schedule(tws, &tws->_state);
    }
    template <typename Pr, typename Func, typename Wrapper>
    void schedule(Pr&& pr, Func&& func, Wrapper&& wrapper) noexcept {
        // If this new throws a std::bad_alloc there is nothing that
        // can be done about it. The corresponding future is not ready
        // and we cannot break the chain. Since this function is
        // noexcept, it will call std::terminate if new throws.
        memory::scoped_critical_alloc_section _;
        auto tws = new continuation<Pr, Func, Wrapper, T SEASTAR_ELLIPSIS>(std::move(pr), std::move(func), std::move(wrapper));
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

    future<T SEASTAR_ELLIPSIS> rethrow_with_nested(future_state_base&& n) noexcept {
        return future<T SEASTAR_ELLIPSIS>(future_state_base::nested_exception_marker(), std::move(n), std::move(_state));
    }

    future<T SEASTAR_ELLIPSIS> rethrow_with_nested() noexcept {
        return future<T SEASTAR_ELLIPSIS>(future_state_base::nested_exception_marker(), std::move(_state));
    }

    template<typename... U>
    friend class shared_future;
public:
    /// \brief The data type carried by the future.
    using value_type = internal::future_stored_type_t<T SEASTAR_ELLIPSIS>;
    using tuple_type = internal::future_tuple_type_t<value_type>;
    /// \brief The data type carried by the future.
    using promise_type = promise<T SEASTAR_ELLIPSIS>;
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

    /// Gets the value returned by the computation.
    ///
    /// Similar to \ref get(), but instead of returning a
    /// tuple, returns the first value of the tuple.  This is
    /// useful for the common case of a \c future<T> with exactly
    /// one type parameter.
    ///
    /// Equivalent to: \c std::get<0>(f.get()).
    using get0_return_type = typename future_state::get0_return_type;
    get0_return_type get0() {
#if SEASTAR_API_LEVEL < 5
        return future_state::get0(get());
#else
        return (get0_return_type)get();
#endif
    }

    /// Wait for the future to be available (in a seastar::thread)
    ///
    /// When called from a seastar::thread, this function blocks the
    /// thread until the future is availble. Other threads and
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
    /// \return \c true if the future is availble and has failed.
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
    template <typename Func, typename Result = futurize_t<typename call_then_impl::template result_type<Func>>>
    SEASTAR_CONCEPT( requires std::invocable<Func, T SEASTAR_ELLIPSIS> || internal::CanInvokeWhenAllSucceed<Func, T SEASTAR_ELLIPSIS>)
    Result
    then(Func&& func) noexcept {
        // The implementation of then() is customized via the call_then_impl helper
        // template, in order to special case the results of when_all_succeed().
        // when_all_succeed() used to return a variadic future, which is deprecated, so
        // now it returns a when_all_succeed_tuple, which we intercept in call_then_impl,
        // and treat it as a variadic future.
#ifndef SEASTAR_TYPE_ERASE_MORE
        return call_then_impl::run(*this, std::move(func));
#else
        using func_type = typename call_then_impl::template func_type<Func>;
        noncopyable_function<func_type> ncf;
        {
            memory::scoped_critical_alloc_section _;
            ncf = noncopyable_function<func_type>([func = std::forward<Func>(func)](auto&&... args) mutable {
                return futurize_invoke(func, std::forward<decltype(args)>(args)...);
            });
        }
        return call_then_impl::run(*this, std::move(ncf));
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
    template <typename Func, typename Result = futurize_t<internal::result_of_apply_t<Func, T SEASTAR_ELLIPSIS>>>
    SEASTAR_CONCEPT( requires ::seastar::CanApplyTuple<Func, T SEASTAR_ELLIPSIS>)
    Result
    then_unpack(Func&& func) noexcept {
        return then([func = std::forward<Func>(func)] (T&& SEASTAR_ELLIPSIS tuple) mutable {
            // sizeof...(tuple) is required to be 1
            return std::apply(func, std::move(tuple) SEASTAR_ELLIPSIS);
        });
    }

private:

    // Keep this simple so that Named Return Value Optimization is used.
    template <typename Func, typename Result>
    Result then_impl_nrvo(Func&& func) noexcept {
        using futurator = futurize<internal::future_result_t<Func, T SEASTAR_ELLIPSIS>>;
        typename futurator::type fut(future_for_get_promise_marker{});
        using pr_type = decltype(fut.get_promise());
        schedule(fut.get_promise(), std::move(func), [](pr_type&& pr, Func& func, future_state&& state) {
            if (state.failed()) {
                pr.set_exception(static_cast<future_state_base&&>(std::move(state)));
            } else {
                futurator::satisfy_with_result_of(std::move(pr), [&func, &state] {
#if SEASTAR_API_LEVEL < 5
                    return std::apply(func, std::move(state).get_value());
#else
                    // clang thinks that "state" is not used, below, for future<>.
                    // Make it think it is used to avoid an unused-lambda-capture warning.
                    (void)state;
                    return internal::future_invoke(func, std::move(state).get_value());
#endif
                });
            }
        });
        return fut;
    }

    template <typename Func, typename Result = futurize_t<internal::future_result_t<Func, T SEASTAR_ELLIPSIS>>>
    Result
    then_impl(Func&& func) noexcept {
#ifndef SEASTAR_DEBUG
        using futurator = futurize<internal::future_result_t<Func, T SEASTAR_ELLIPSIS>>;
        if (failed()) {
            return futurator::make_exception_future(static_cast<future_state_base&&>(get_available_state_ref()));
        } else if (available()) {
#if SEASTAR_API_LEVEL < 5
            return futurator::apply(std::forward<Func>(func), get_available_state_ref().take_value());
#else
            return futurator::invoke(std::forward<Func>(func), get_available_state_ref().take_value());
#endif
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
    template <typename Func, typename FuncResult = std::invoke_result_t<Func, future>>
    SEASTAR_CONCEPT( requires std::invocable<Func, future> )
    futurize_t<FuncResult>
    then_wrapped(Func&& func) & noexcept {
        return then_wrapped_maybe_erase<false, FuncResult>(std::forward<Func>(func));
    }

    template <typename Func, typename FuncResult = std::invoke_result_t<Func, future&&>>
    SEASTAR_CONCEPT( requires std::invocable<Func, future&&> )
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

    void forward_to(internal::promise_base_with_type<T SEASTAR_ELLIPSIS>&& pr) noexcept {
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
    void forward_to(promise<T SEASTAR_ELLIPSIS>&& pr) noexcept {
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
     * If both of them are exceptional - the std::nested_exception exception
     * with the callback exception on top and the original future exception
     * nested will be propagated.
     */
    template <typename Func>
    SEASTAR_CONCEPT( requires std::invocable<Func> )
    future<T SEASTAR_ELLIPSIS> finally(Func&& func) noexcept {
        return then_wrapped(finally_body<Func, is_future<std::invoke_result_t<Func>>::value>(std::forward<Func>(func)));
    }


    template <typename Func, bool FuncReturnsFuture>
    struct finally_body;

    template <typename Func>
    struct finally_body<Func, true> {
        Func _func;

        finally_body(Func&& func) noexcept : _func(std::forward<Func>(func))
        { }

        future<T SEASTAR_ELLIPSIS> operator()(future<T SEASTAR_ELLIPSIS>&& result) noexcept {
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

        future<T SEASTAR_ELLIPSIS> operator()(future<T SEASTAR_ELLIPSIS>&& result) noexcept {
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
    SEASTAR_CONCEPT( requires ::seastar::InvokeReturns<Func, future<T SEASTAR_ELLIPSIS>, std::exception_ptr>
                    || (std::tuple_size_v<tuple_type> == 0 && ::seastar::InvokeReturns<Func, void, std::exception_ptr>)
                    || (std::tuple_size_v<tuple_type> == 1 && ::seastar::InvokeReturns<Func, T, std::exception_ptr>)
                    || (std::tuple_size_v<tuple_type> > 1 && ::seastar::InvokeReturns<Func, tuple_type, std::exception_ptr>)
    )
    future<T SEASTAR_ELLIPSIS> handle_exception(Func&& func) noexcept {
        return then_wrapped([func = std::forward<Func>(func)]
                             (auto&& fut) mutable -> future<T SEASTAR_ELLIPSIS> {
            if (!fut.failed()) {
                return make_ready_future<T SEASTAR_ELLIPSIS>(fut.get());
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
    future<T SEASTAR_ELLIPSIS> handle_exception_type(Func&& func) noexcept {
        using trait = function_traits<Func>;
        static_assert(trait::arity == 1, "func can take only one parameter");
        using ex_type = typename trait::template arg<0>::type;
        return then_wrapped([func = std::forward<Func>(func)]
                             (auto&& fut) mutable -> future<T SEASTAR_ELLIPSIS> {
            try {
                return make_ready_future<T SEASTAR_ELLIPSIS>(fut.get());
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

#ifdef SEASTAR_COROUTINES_ENABLED
    using future_base::set_coroutine;
#endif
private:
    void set_callback(continuation_base<T SEASTAR_ELLIPSIS>* callback) noexcept {
        if (_state.available()) {
            callback->set_state(get_available_state_ref());
            ::seastar::schedule(callback);
        } else {
            assert(_promise);
            schedule(callback);
        }

    }

    /// \cond internal
    template <typename SEASTAR_ELLIPSIS U>
    friend class future;
    template <typename SEASTAR_ELLIPSIS U>
    friend class promise;
    template <typename U>
    friend struct futurize;
    template <typename SEASTAR_ELLIPSIS U>
    friend class internal::promise_base_with_type;
    template <typename... U, typename... A>
    friend future<U...> make_ready_future(A&&... value) noexcept;
    template <typename... U>
    friend future<U...> make_exception_future(std::exception_ptr&& ex) noexcept;
    template <typename... U, typename Exception>
    friend future<U...> make_exception_future(Exception&& ex) noexcept;
    template <typename... U>
    friend future<U...> internal::make_exception_future(future_state_base&& state) noexcept;
    template <typename... U>
    friend future<U...> current_exception_as_future() noexcept;
    template <typename... U, typename V>
    friend void internal::set_callback(future<U...>&&, V*) noexcept;
    template <typename Future>
    friend struct internal::call_then_impl;
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

    /// Deprecated alias of invoke
    template<typename Func, typename... FuncArgs>
    [[deprecated("Use invoke for varargs")]]
    static inline type apply(Func&& func, FuncArgs&&... args) noexcept {
        return invoke(std::forward<Func>(func), std::forward<FuncArgs>(args)...);
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

#if SEASTAR_API_LEVEL >= 5
    /// Convert the tuple representation into a future
    static type from_tuple(value_type&& value) {
        return type(ready_future_marker(), std::move(value));
    }
    /// Convert the tuple representation into a future
    static type from_tuple(const value_type& value) {
        return type(ready_future_marker(), value);
    }
#endif
private:
    /// Forwards the result of, or exception thrown by, func() to the
    /// promise. This avoids creating a future if func() doesn't
    /// return one.
    template<typename Func>
    SEASTAR_CONCEPT( requires std::invocable<Func> )
    static void satisfy_with_result_of(promise_base_with_type&&, Func&& func);

    template <typename SEASTAR_ELLIPSIS U>
    friend class future;
};

inline internal::promise_base::promise_base(future_base* future, future_state_base* state) noexcept
    : _future(future), _state(state) {
    _future->_promise = this;
}

template <typename SEASTAR_ELLIPSIS T>
inline
future<T SEASTAR_ELLIPSIS>
promise<T SEASTAR_ELLIPSIS>::get_future() noexcept {
    assert(!this->_future && this->_state && !this->_task);
    return future<T SEASTAR_ELLIPSIS>(this);
}

template <typename SEASTAR_ELLIPSIS T>
inline
void promise<T SEASTAR_ELLIPSIS>::move_it(promise&& x) noexcept {
    if (this->_state == &x._local_state) {
        this->_state = &_local_state;
        new (&_local_state) future_state(std::move(x._local_state));
    }
}

template <typename... T, typename... A>
inline
future<T...> make_ready_future(A&&... value) noexcept {
    return future<T...>(ready_future_marker(), std::forward<A>(value)...);
}

template <typename... T>
inline
future<T...> make_exception_future(std::exception_ptr&& ex) noexcept {
    return future<T...>(exception_future_marker(), std::move(ex));
}

template <typename... T>
inline
future<T...> internal::make_exception_future(future_state_base&& state) noexcept {
    return future<T...>(exception_future_marker(), std::move(state));
}

template <typename... T>
future<T...> current_exception_as_future() noexcept {
    return future<T...>(future_state_base::current_exception_future_marker());
}

void log_exception_trace() noexcept;

/// \brief Creates a \ref future in an available, failed state.
///
/// Creates a \ref future object that is already resolved in a failed
/// state.  This no I/O needs to be performed to perform a computation
/// (for example, because the connection is closed and we cannot read
/// from it).
template <typename... T, typename Exception>
inline
future<T...> make_exception_future(Exception&& ex) noexcept {
    log_exception_trace();
    return make_exception_future<T...>(std::make_exception_ptr(std::forward<Exception>(ex)));
}

template <typename... T, typename Exception>
future<T...> make_exception_future_with_backtrace(Exception&& ex) noexcept {
    return make_exception_future<T...>(make_backtraced_exception_ptr<Exception>(std::forward<Exception>(ex)));
}

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
template<typename Func>
SEASTAR_CONCEPT( requires std::invocable<Func> )
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
[[deprecated("Use futurize_invoke for varargs")]]
auto futurize_apply(Func&& func, Args&&... args) noexcept {
    return futurize_invoke(std::forward<Func>(func), std::forward<Args>(args)...);
}

template<typename Func, typename... Args>
auto futurize_apply(Func&& func, std::tuple<Args...>&& args) noexcept {
    using futurator = futurize<std::invoke_result_t<Func, Args&&...>>;
    return futurator::apply(std::forward<Func>(func), std::move(args));
}

namespace internal {

template <typename... T, typename U>
inline
void set_callback(future<T...>&& fut, U* callback) noexcept {
    // It would be better to use continuation_base<T...> for U, but
    // then a derived class of continuation_base<T...> won't be matched
    return std::move(fut).set_callback(callback);
}

}


/// \endcond

}
