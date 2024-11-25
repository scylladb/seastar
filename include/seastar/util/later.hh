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
 * Copyright (C) 2020 ScyllaDB.
 */


#pragma once

#include <seastar/core/future.hh>

namespace seastar {

/// \addtogroup future-util
/// @{

/// \brief Returns a ready future.
inline
future<> now() {
    return make_ready_future<>();
}

/// \brief Returns a future which is not ready but is scheduled to resolve soon.
///
/// Schedules a future to run "soon". yield() can be used to break long-but-finite
/// loops into pieces. Note that if nothing else is runnable,
/// It will not check for I/O, and so an infinite loop with yield() will just
/// burn CPU.
future<> yield() noexcept;

/// Yield the cpu if the task quota is exhausted.
///
/// Check if the current continuation is preempted and yield if so. Otherwise
/// return a ready future.
///
/// \note Threads and coroutines (see seastar::thread::maybe_yield() and
///       seastar::coroutine::maybe_yield() have their own custom variants,
///       and the various continuation-based loops (do_for_each() and similar)
///       do this automatically.
inline
future<> maybe_yield() noexcept {
    if (need_preempt()) {
        return yield();
    } else {
        return make_ready_future<>();
    }
}

/// Force the reactor to check for pending I/O
///
/// Schedules a check for new I/O completions (disk operations completions
/// or network packet arrival) immediately and return a future that is ready
/// when the I/O has been polled for.
///
/// \note It is very rare to need to call this function. It is better to let the
///       reactor schedule I/O polls itself.
/// \note This has no effect on I/O polling on other shards.
future<> check_for_io_immediately() noexcept;

/// \brief Returns a future which is not ready but is scheduled to resolve soon.
///
/// \deprecated Use yield() instead, or check_for_io_immediately() if your really need it.
[[deprecated("Use yield() or check_for_io_immediately()")]]
future<> later() noexcept;

/// @}

} // namespace seastar
