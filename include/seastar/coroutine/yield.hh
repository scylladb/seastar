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
 * Copyright (C) 2026-present ScyllaDB
 */

#pragma once

#include <seastar/core/coroutine.hh>

namespace seastar::coroutine {

namespace internal {

struct yield_awaiter final {
    bool await_ready() const noexcept {
        return false;
    }

    template <typename T>
    void await_suspend(std::coroutine_handle<T> h SEASTAR_COROUTINE_LOC_PARAM) noexcept {
        SEASTAR_COROUTINE_LOC_STORE(h.promise());
        schedule(&h.promise());
    }

    void await_resume() noexcept {
    }
};

}

/// Unconditionally yield to the reactor.
///
/// `co_await coroutine::yield()` puts the current coroutine at the back of
/// the task queue of its scheduling group, letting every task already queued
/// there run before the coroutine resumes.
///
/// It is the coroutine counterpart of \ref seastar::yield(), and costs one
/// task per yield rather than two: the coroutine frame is scheduled directly,
/// with no intermediate task and future to carry the continuation.
///
/// Use \ref seastar::coroutine::maybe_yield() instead when the yield only
/// needs to happen once the task quota is exhausted.
///
/// Example
///
/// ```
/// seastar::future<> poll_until_done(state& s) {
///     while (!s.done()) {
///         s.poll();
///         co_await seastar::coroutine::yield();
///     }
/// }
/// ```
class [[nodiscard("must co_await a yield() object")]] yield {
public:
    auto operator co_await() { return internal::yield_awaiter(); }
};

}
