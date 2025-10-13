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

#include <seastar/core/scheduling.hh>
#include <seastar/core/slim_source_location.hh>
#include <seastar/util/backtrace.hh>

#ifndef SEASTAR_MODULE
#include <utility>
#endif

namespace seastar {

SEASTAR_MODULE_EXPORT
class task {
    slim_source_location _resume_point = {};

protected:
    scheduling_group _sg;

private:
#ifdef SEASTAR_TASK_BACKTRACE
    shared_backtrace _bt;
#endif
protected:
    // Task destruction is performed by run_and_dispose() via a concrete type,
    // so no need for a virtual destructor here. Derived classes that implement
    // run_and_dispose() should be declared final to avoid losing concrete type
    // information via inheritance.
    ~task() = default;

    scheduling_group set_scheduling_group(scheduling_group new_sg) noexcept{
        return std::exchange(_sg, new_sg);
    }
public:
    explicit task(scheduling_group sg = current_scheduling_group()) noexcept : _sg(sg) {}
    virtual void run_and_dispose() noexcept = 0;
    /// Returns the next task which is waiting for this task to complete execution, or nullptr.
    virtual task* waiting_task() noexcept = 0;
    void update_resume_point(slim_source_location sl) { _resume_point = sl; }
    auto get_resume_point() const { return _resume_point; }
    scheduling_group group() const { return _sg; }
#ifdef SEASTAR_TASK_BACKTRACE
    void make_backtrace() noexcept;
    shared_backtrace get_backtrace() const { return _bt; }
#else
    void make_backtrace() noexcept {}
    shared_backtrace get_backtrace() const { return {}; }
#endif
};

SEASTAR_MODULE_EXPORT_BEGIN

void schedule(task* t) noexcept;
void schedule_checked(task* t) noexcept;
void schedule_urgent(task* t) noexcept;

SEASTAR_MODULE_EXPORT_END
}
