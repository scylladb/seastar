/*
* This file is open source software, licensed to you under the terms
* of the Apache License, Version 2.0 (the "License"). See the NOTICE file
* distributed with this work for additional information regarding copyright
* ownership. You may not use this file except in compliance with the License.
*
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing,
* software distributed under the License is distributed on an
* "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
* KIND, either express or implied. See the License for the
* specific language governing permissions and limitations
* under the License.
*/
/*
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <cstddef>
#endif
#include <seastar/core/semaphore.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/modules.hh>

namespace seastar {

/// \cond internal
// lock / unlock semantics for rwlock, so it can be used with with_lock()
SEASTAR_MODULE_EXPORT
template<typename Clock>
class basic_rwlock;

template<typename Clock = typename timer<>::clock>
class rwlock_for_read {
public:
    future<> lock() {
        return static_cast<basic_rwlock<Clock>*>(this)->read_lock();
    }
    void unlock() {
        static_cast<basic_rwlock<Clock>*>(this)->read_unlock();
    }
    friend class basic_rwlock<Clock>;
};

template<typename Clock = typename timer<>::clock>
class rwlock_for_write {
public:
    future<> lock() {
        return static_cast<basic_rwlock<Clock>*>(this)->write_lock();
    }
    void unlock() {
        static_cast<basic_rwlock<Clock>*>(this)->write_unlock();
    }
    friend class basic_rwlock<Clock>;
};
/// \endcond


/// \addtogroup fiber-module
/// @{

/// Implements a read-write lock mechanism. Beware: this is not a cross-CPU
/// lock, due to seastar's sharded architecture.
/// Instead, it can be used to achieve rwlock semantics between two (or more)
/// fibers running in the same CPU that may use the same resource.
/// Acquiring the write lock will effectively cause all readers not to be executed
/// until the write part is done.
SEASTAR_MODULE_EXPORT
template<typename Clock = typename timer<>::clock>
class basic_rwlock : private rwlock_for_read<Clock>, rwlock_for_write<Clock> {
    using semaphore_type = basic_semaphore<semaphore_default_exception_factory, Clock>;

    static constexpr size_t max_ops = semaphore_type::max_counter();

    semaphore_type _sem;
public:
    basic_rwlock()
            : _sem(max_ops) {
    }

    /// Cast this rwlock into read lock object with lock semantics appropriate to be used
    /// by "with_lock". The resulting object will have lock / unlock calls that, when called,
    /// will acquire / release the lock in read mode.
    rwlock_for_read<Clock>& for_read() {
        return *this;
    }

    /// Cast this rwlock into write lock object with lock semantics appropriate to be used
    /// by "with_lock". The resulting object will have lock / unlock calls that, when called,
    /// will acquire / release the lock in write mode.
    rwlock_for_write<Clock>& for_write() {
        return *this;
    }

    /// Acquires this lock in read mode. Many readers are allowed, but when
    /// this future returns, and until \ref read_unlock is called, all fibers
    /// waiting on \ref write_lock are guaranteed not to execute.
    future<> read_lock(typename semaphore_type::time_point timeout = semaphore_type::time_point::max()) {
        return _sem.wait(timeout);
    }

    future<> read_lock(abort_source& as) {
        return _sem.wait(as);
    }

    /// Releases the lock, which must have been taken in read mode. After this
    /// is called, one of the fibers waiting on \ref write_lock will be allowed
    /// to proceed.
    void read_unlock() {
        SEASTAR_ASSERT(_sem.current() < max_ops);
        _sem.signal();
    }

    /// Acquires this lock in write mode. Only one writer is allowed. When
    /// this future returns, and until \ref write_unlock is called, all other
    /// fibers waiting on either \ref read_lock or \ref write_lock are guaranteed
    /// not to execute.
    future<> write_lock(typename semaphore_type::time_point timeout = semaphore_type::time_point::max()) {
        return _sem.wait(timeout, max_ops);
    }

    future<> write_lock(abort_source& as) {
        return _sem.wait(as, max_ops);
    }

    /// Releases the lock, which must have been taken in write mode. After this
    /// is called, one of the other fibers waiting on \ref write_lock or the fibers
    /// waiting on \ref read_lock will be allowed to proceed.
    void write_unlock() {
        SEASTAR_ASSERT(_sem.current() == 0);
        _sem.signal(max_ops);
    }

    /// Tries to acquire the lock in read mode iff this can be done without waiting.
    bool try_read_lock() {
        return _sem.try_wait();
    }

    /// Tries to acquire the lock in write mode iff this can be done without waiting.
    bool try_write_lock() {
        return _sem.try_wait(max_ops);
    }

    using holder = semaphore_units<semaphore_default_exception_factory, Clock>;

    /// hold_read_lock() waits for a read lock and returns an object which,
    /// when destroyed, releases the lock. This makes it easy to ensure that
    /// the lock is eventually undone, at any circumstance (even including
    /// exceptions). The release() method can be used on the returned object
    /// to release its ownership of the lock and avoid the automatic unlock.
    /// Note that both hold_read_lock() and hold_write_lock() return an object
    /// of the same type, rwlock::holder.
    ///
    /// hold_read_lock() may throw an exception (or, in other implementations,
    /// return an exceptional future) when it failed to obtain the lock -
    /// e.g., on allocation failure.
    future<holder> hold_read_lock(typename semaphore_type::time_point timeout = semaphore_type::time_point::max()) {
        return get_units(_sem, 1, timeout);
    }

    future<holder> hold_read_lock(abort_source& as) {
        return get_units(_sem, 1, as);
    }

    /// try_hold_read_lock() synchronously tries to get a read lock and, if successful, returns an
    /// optional object which, when destroyed, releases the lock.
    std::optional<holder> try_hold_read_lock() noexcept {
        return try_get_units(_sem, 1);
    }

    /// hold_write_lock() waits for a write lock and returns an object which,
    /// when destroyed, releases the lock. This makes it easy to ensure that
    /// the lock is eventually undone, at any circumstance (even including
    /// exceptions). The release() method can be used on the returned object
    /// to release its ownership of the lock and avoid the automatic unlock.
    /// Note that both hold_read_lock() and hold_write_lock() return an object
    /// of the same type, rwlock::holder.
    ///
    /// hold_read_lock() may throw an exception (or, in other implementations,
    /// return an exceptional future) when it failed to obtain the lock -
    /// e.g., on allocation failure.
    future<holder> hold_write_lock(typename semaphore_type::time_point timeout = semaphore_type::time_point::max()) {
        return get_units(_sem, max_ops, timeout);
    }

    future<holder> hold_write_lock(abort_source& as) {
        return get_units(_sem, max_ops, as);
    }

    /// try_hold_write_lock() synchronously tries to get a write lock and, if successful, returns an
    /// optional object which, when destroyed, releases the lock.
    std::optional<holder> try_hold_write_lock() noexcept {
        return try_get_units(_sem, max_ops);
    }

    /// Checks if any read or write locks are currently held.
    bool locked() const {
        return _sem.available_units() != max_ops;
    }

    friend class rwlock_for_read<Clock>;
    friend class rwlock_for_write<Clock>;
};

SEASTAR_MODULE_EXPORT
using rwlock = basic_rwlock<>;

/// @}

}
