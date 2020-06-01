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

#include <memory>
#include <seastar/core/scheduling.hh>
#include <seastar/util/backtrace.hh>

namespace seastar {

class task {
    scheduling_group _sg;
#ifdef SEASTAR_TASK_BACKTRACE
    shared_backtrace _bt;
#endif
protected:
    // Task destruction is performed by run_and_dispose() via a concrete type,
    // so no need for a virtual destructor here. Derived classes that implement
    // run_and_dispose() should be declared final to avoid losing concrete type
    // information via inheritance.
    ~task() = default;
public:
    explicit task(scheduling_group sg = current_scheduling_group()) noexcept : _sg(sg) {}
    virtual void run_and_dispose() noexcept = 0;
    /// Returns the next task which is waiting for this task to complete execution, or nullptr.
    virtual task* waiting_task() noexcept = 0;
    scheduling_group group() const { return _sg; }
    shared_backtrace get_backtrace() const;
#ifdef SEASTAR_TASK_BACKTRACE
    void make_backtrace() noexcept;
#else
    void make_backtrace() noexcept {}
#endif
};

inline
shared_backtrace task::get_backtrace() const {
#ifdef SEASTAR_TASK_BACKTRACE
    return _bt;
#else
    return {};
#endif
}

void schedule(task* t) noexcept;
void schedule_urgent(task* t) noexcept;

}
