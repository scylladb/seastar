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
 * Copyright (C) 2022-present ScyllaDB
 */

#pragma once

#include <seastar/core/coroutine.hh>
#include <seastar/core/scheduling.hh>

namespace seastar::coroutine {

/// Switch the current task scheduling group.
///
/// `switch_to(new_scheduling_group)` can be used to change
/// the \ref scheduling_group of the currently running coroutine.
///
/// If the new scheduling group is different than the current scheduling_group,
/// the coroutine is re-scheduled using the new scheduling group.
/// Otherwise, the coroutine just continues to run with
/// the current scheduling group.
///
/// `switch_to` returns the current scheduling group
/// to make it easy to switch back to it if needed.
///
/// Example
///
/// ```
/// seastar::future<> cor() {
///     ... // do some preliminary work
///     auto prev_sg = co_await coroutine::switch_to(other_sg);
///     ... // do some work using another scheduling group
///     co_await coroutine::switch_to(prev_sg);
///     ... // do some more work
///     co_return;
/// }
/// ```

struct [[nodiscard("must co_await a switch_to object")]] switch_to final : task {
    scheduling_group _prev_sg;
    scheduling_group _switch_to_sg;
    task* _task = nullptr;
public:
    explicit switch_to(scheduling_group new_sg) noexcept
        : _prev_sg(current_scheduling_group())
        , _switch_to_sg(std::move(new_sg))
    { }

    switch_to(const switch_to&) = delete;
    switch_to(switch_to&&) = delete;

    bool await_ready() const noexcept {
        return current_scheduling_group() == _switch_to_sg;
    }

    template<typename T>
    void await_suspend(std::coroutine_handle<T> hndl) noexcept {
        auto& t = hndl.promise();
        t.set_scheduling_group(_switch_to_sg);
        _task = &t;
        schedule(_task);
    }

    scheduling_group await_resume() {
        return _prev_sg;
    }

    virtual void run_and_dispose() noexcept override { }

    virtual task* waiting_task() noexcept override {
        return _task;
    }
};

} // namespace seastar::coroutine
