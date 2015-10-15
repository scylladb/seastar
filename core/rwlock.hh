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

#ifndef CORE_RWLOCK_HH_
#define CORE_RWLOCK_HH_

#include "semaphore.hh"

/// \addtogroup fiber-module
/// @{

/// Implements a read-write lock mechanism. Beware: this is not a cross-CPU
/// lock, due to seastar's sharded architecture.
/// Instead, it can be used to achieve rwlock semantics between two (or more)
/// fibers running in the same CPU that may use the same resource.
/// Acquiring the write lock will effectively cause all readers not to be executed
/// until the write part is done.
class rwlock {
    static const size_t max_ops = std::numeric_limits<size_t>::max();

    semaphore _sem;
public:
    rwlock()
            : _sem(max_ops) {
    }

    /// Acquires this lock in read mode. Many readers are allowed, but when
    /// this future returns, and until \ref read_unlock is called, all fibers
    /// waiting on \ref write_lock are guaranteed not to execute.
    future<> read_lock() {
        return _sem.wait();
    }

    /// Releases the lock, which must have been taken in read mode. After this
    /// is called, one of the fibers waiting on \ref write_lock will be allowed
    /// to proceed.
    void read_unlock() {
        _sem.signal();
    }

    /// Acquires this lock in write mode. Only one writer is allowed. When
    /// this future returns, and until \ref write_unlock is called, all other
    /// fibers waiting on either \ref read_lock or \ref write_lock are guaranteed
    /// not to execute.
    future<> write_lock() {
        return _sem.wait(max_ops);
    }

    /// Releases the lock, which must have been taken in write mode. After this
    /// is called, one of the other fibers waiting on \ref write_lock or the fibers
    /// waiting on \ref read_lock will be allowed to proceed.
    void write_unlock() {
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
};
/// @}
}
#endif /* CORE_RWLOCK_HH_ */
