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
 * Copyright 2020 ScyllaDB
 */

#pragma once

#include <seastar/util/noncopyable_function.hh>

/// \file

namespace seastar {

/// Indicates the outcome of a user callback installed to take advantage of
/// idle CPU cycles.
enum class idle_cpu_handler_result {
    no_more_work,                       //!< The user callback has no more work to perform
    interrupted_by_higher_priority_task //!< A call to the work_waiting_on_reactor parameter to idle_cpu_handler returned `true`
};

/// Signature of a callback provided by the reactor to a user callback installed to take
/// advantage of idle cpu cycles, used to periodically check if the CPU is still idle.
///
/// \return true if the reactor has new work to do
using work_waiting_on_reactor = const noncopyable_function<bool()>&;

/// Signature of a callback provided by the user, that the reactor calls when it has idle cycles.
///
/// The `poll` parameter is a work_waiting_on_reactor function that should be periodically called
/// to check if the idle callback should return with idle_cpu_handler_result::interrupted_by_higher_priority_task
using idle_cpu_handler = noncopyable_function<idle_cpu_handler_result(work_waiting_on_reactor poll)>;

/// Set a handler that will be called when there is no task to execute on cpu.
/// Handler should do a low priority work.
///
/// Handler's return value determines whether handler did any actual work. If no work was done then reactor will go
/// into sleep.
///
/// Handler's argument is a function that returns true if a task which should be executed on cpu appears or false
/// otherwise. This function should be used by a handler to return early if a task appears.
void set_idle_cpu_handler(idle_cpu_handler&& handler);

}
