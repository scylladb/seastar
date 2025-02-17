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
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/chunked_fifo.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/modules.hh>
#include <mutex>
#include <utility>
#include <shared_mutex>
#endif

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

/// \addtogroup fiber-module
/// @{

/// \brief Shared/exclusive mutual exclusion.
///
/// Similar to \c std::shared_mutex, this class provides protection
/// for a shared resource, with two levels of access protection: shared
/// and exclusive.  Shared access allows multiple tasks to access the
/// shared resource concurrently, while exclusive access allows just
/// one task to access the resource at a time.
///
/// Note that many seastar tasks do not require protection at all,
/// since the seastar scheduler is not preemptive; however tasks that do
/// (by waiting on a future) may require explicit locking.
///
/// The \ref with_shared(shared_mutex&, Func&&) and
/// \ref with_lock(shared_mutex&, Func&&) provide exception-safe
/// wrappers for use with \c shared_mutex.
///
/// \see semaphore simpler mutual exclusion
class shared_mutex {
    unsigned _readers = 0;
    bool _writer = false;
    struct waiter {
        waiter(promise<>&& pr, bool for_write) : pr(std::move(pr)), for_write(for_write) {}
        promise<> pr;
        bool for_write;
    };
    chunked_fifo<waiter> _waiters;
public:
    shared_mutex() = default;
    shared_mutex(shared_mutex&&) = default;
    shared_mutex& operator=(shared_mutex&&) = default;
    shared_mutex(const shared_mutex&) = delete;
    void operator=(const shared_mutex&) = delete;
    /// Lock the \c shared_mutex for shared access
    ///
    /// \return a future that becomes ready when no exclusive access
    ///         is granted to anyone.
    future<> lock_shared() noexcept {
        if (try_lock_shared()) {
            return make_ready_future<>();
        }
        try {
            _waiters.emplace_back(promise<>(), false);
            return _waiters.back().pr.get_future();
        } catch (...) {
            return current_exception_as_future();
        }
    }
    /// Try to lock the \c shared_mutex for shared access
    ///
    /// \return true iff could acquire the lock for shared access.
    bool try_lock_shared() noexcept {
        if (!_writer && _waiters.empty()) {
            ++_readers;
            return true;
        }
        return false;
    }
    /// Unlocks a \c shared_mutex after a previous call to \ref lock_shared().
    void unlock_shared() noexcept {
        SEASTAR_ASSERT(_readers > 0);
        --_readers;
        wake();
    }
    /// Lock the \c shared_mutex for exclusive access
    ///
    /// \return a future that becomes ready when no access, shared or exclusive
    ///         is granted to anyone.
    future<> lock() noexcept {
        if (try_lock()) {
            return make_ready_future<>();
        }
        try {
            _waiters.emplace_back(promise<>(), true);
            return _waiters.back().pr.get_future();
        } catch (...) {
            return current_exception_as_future();
        }
    }
    /// Try to lock the \c shared_mutex for exclusive access
    ///
    /// \return true iff could acquire the lock for exclusive access.
    bool try_lock() noexcept {
        if (!_readers && !_writer) {
            _writer = true;
            return true;
        }
        return false;
    }
    /// Unlocks a \c shared_mutex after a previous call to \ref lock().
    void unlock() noexcept {
        SEASTAR_ASSERT(_writer);
        _writer = false;
        wake();
    }
private:
    void wake() noexcept {
        while (!_waiters.empty()) {
            auto& w = _waiters.front();
            // note: _writer == false in wake()
            if (w.for_write) {
                if (!_readers) {
                    _writer = true;
                    w.pr.set_value();
                    _waiters.pop_front();
                }
                break;
            } else { // for read
                ++_readers;
                w.pr.set_value();
                _waiters.pop_front();
            }
        }
    }
};

/// Executes a function while holding shared access to a resource.
///
/// Executes a function while holding shared access to a resource.  When
/// the function returns, the mutex is automatically unlocked.
///
/// \param sm a \ref shared_mutex guarding access to the shared resource
/// \param func callable object to invoke while the mutex is held for shared access
/// \return whatever \c func returns, as a future
///
/// \relates shared_mutex
template <std::invocable Func>
    requires std::is_nothrow_move_constructible_v<Func>
    inline
    futurize_t<std::invoke_result_t<Func>>
with_shared(shared_mutex& sm, Func&& func) noexcept {
    return sm.lock_shared().then([&sm, func = std::forward<Func>(func)] () mutable {
        return futurize_invoke(func).finally([&sm] {
            sm.unlock_shared();
        });
    });
}

template <std::invocable Func>
    requires (!std::is_nothrow_move_constructible_v<Func>)
    inline
    futurize_t<std::invoke_result_t<Func>>
with_shared(shared_mutex& sm, Func&& func) noexcept {
    // FIXME: use a coroutine when c++17 support is dropped
    try {
        return do_with(std::forward<Func>(func), [&sm] (Func& func) {
            return sm.lock_shared().then([&func] {
                return func();
            }).finally([&sm] {
                sm.unlock_shared();
            });
        });
    } catch (...) {
        return futurize<std::invoke_result_t<Func>>::current_exception_as_future();
    }
}

/// Executes a function while holding exclusive access to a resource.
///
/// Executes a function while holding exclusive access to a resource.  When
/// the function returns, the mutex is automatically unlocked.
///
/// \param sm a \ref shared_mutex guarding access to the shared resource
/// \param func callable object to invoke while the mutex is held for shared access
/// \return whatever \c func returns, as a future
///
/// \relates shared_mutex
template <std::invocable Func>
    requires std::is_nothrow_move_constructible_v<Func>
    inline
    futurize_t<std::invoke_result_t<Func>>
with_lock(shared_mutex& sm, Func&& func) noexcept {
    return sm.lock().then([&sm, func = std::forward<Func>(func)] () mutable {
        return futurize_invoke(func).finally([&sm] {
            sm.unlock();
        });
    });
}


template <std::invocable Func>
    requires (!std::is_nothrow_move_constructible_v<Func>)
    inline
    futurize_t<std::invoke_result_t<Func>>
with_lock(shared_mutex& sm, Func&& func) noexcept {
    // FIXME: use a coroutine when c++17 support is dropped
    try {
        return do_with(std::forward<Func>(func), [&sm] (Func& func) {
            return sm.lock().then([&func] {
                return func();
            }).finally([&sm] {
                sm.unlock();
            });
        });
    } catch (...) {
        return futurize<std::invoke_result_t<Func>>::current_exception_as_future();
    }
}

namespace internal {

/// Seastar analogue of the `BasicLockable` named requirement, see:
///   [https://eel.is/c++draft/thread.req.lockable.basic#def:Cpp17BasicLockable]
template <typename T>
concept FutureBasicLockable = requires (T& t) {
    { t.lock() } -> std::same_as<future<>>;
    { t.unlock() } noexcept -> std::same_as<void>;
};

/// Seastar analogue of the `SharedLockable` named requirement, see:
///   [https://eel.is/c++draft/thread.req.lockable.shared#def:Cpp17SharedLockable]
/// It's called `basic` because the concept doesn't require the type
/// to define `try_lock_shared()` method to satisfy the constraint, similarly
/// to the `BasicLockable` named requirement.
template <typename T>
concept FutureBasicSharedLockable = requires (T& t) {
    { t.lock_shared() } -> std::same_as<future<>>;
    { t.unlock_shared() } noexcept -> std::same_as<void>;
};

} // namespace internal

/// \brief Construct a RAII-based shared lock corresponding to a given object.
///
/// Since constructors cannot be exectued asynchronously, use this function
/// to shared lock the passed object and construct an std::shared_lock
/// that will unlock it when the lock is being destroyed.
///
/// The caller is responsible for keeping the passed object
/// alive at least until the returned lock is destroyed.
template <internal::FutureBasicSharedLockable T>
future<std::shared_lock<T>> get_shared_lock(T& t) {
    co_await t.lock_shared();

    try {
        co_return std::shared_lock<T>{t, std::adopt_lock_t{}};
    } catch (...) {
        t.unlock_shared();
        throw;
    }
}

/// \brief Construct a RAII-based unique lock corresponding to a given object.
///
/// Since constructors cannot be exectued asynchronously, use this function
/// to lock the passed object and construct an std::unique_lock
/// that will unlock it when the lock is being destroyed.
///
/// The caller is responsible for keeping the passed object
/// alive at least until the returned lock is destroyed.
template <internal::FutureBasicLockable T>
future<std::unique_lock<T>> get_unique_lock(T& t) {
    co_await t.lock();

    try {
        co_return std::unique_lock<T>{t, std::adopt_lock_t{}};
    } catch (...) {
        t.unlock();
        throw;
    }
}

/// @}
SEASTAR_MODULE_EXPORT_END

}
