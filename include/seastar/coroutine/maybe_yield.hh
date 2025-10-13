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
 * Copyright (C) 2021-present ScyllaDB
 */

#pragma once

#include <concepts>
#include <type_traits>
#include <seastar/core/coroutine.hh>

namespace seastar::coroutine {

namespace internal {

struct maybe_yield_awaiter final {
    bool await_ready() const {
        return !need_preempt();
    }

    template <typename T>
    void await_suspend(std::coroutine_handle<T> h, slim_source_location sl = {}) noexcept {
        h.promise().update_resume_point(sl);
        schedule(&h.promise());
    }

    void await_resume() {
    }
};

}

/// Preempt if the current task quota expired.
///
/// `maybe_yield()` can be used to break a long computation in a
/// coroutine and allow the reactor to preempt its execution. This
/// allows other tasks to gain access to the CPU. If the task quota
/// did not expire, the coroutine continues execution.
///
/// It should be used in long loops that do not contain other `co_await`
/// calls.
///
/// Example
///
/// ```
/// seastar::future<int> long_loop(int n) {
///     float acc = 0;
///     for (int i = 0; i < n; ++i) {
///         acc += std::sin(float(i));
///         co_await seastar::coroutine::maybe_yield();
///     }
///     co_return acc;
/// }
/// ```
class [[nodiscard("must co_await an maybe_yield() object")]] maybe_yield {
public:
    auto operator co_await() { return internal::maybe_yield_awaiter(); }
};

}
