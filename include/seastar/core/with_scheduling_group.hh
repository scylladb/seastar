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
#include <seastar/core/make_task.hh>
#include <concepts>
#include <tuple>
#include <utility>

namespace seastar {

/// \addtogroup future-util
/// @{

namespace internal {

template <typename Func>
SEASTAR_CONCEPT( requires std::is_nothrow_move_constructible_v<Func> )
auto
schedule_in_group(scheduling_group sg, Func func) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<Func>);
    auto tsk = make_task(sg, std::move(func));
    schedule(tsk);
    return tsk->get_future();
}


}

/// \brief run a callable (with some arbitrary arguments) in a scheduling group
///
/// If the conditions are suitable (see scheduling_group::may_run_immediately()),
/// then the function is run immediately. Otherwise, the function is queued to run
/// when its scheduling group next runs.
///
/// \param sg  scheduling group that controls execution time for the function
/// \param func function to run; must be movable or copyable
/// \param args arguments to the function; may be copied or moved, so use \c std::ref()
///             to force passing references
template <typename Func, typename... Args>
SEASTAR_CONCEPT( requires std::is_nothrow_move_constructible_v<Func> )
inline
auto
with_scheduling_group(scheduling_group sg, Func func, Args&&... args) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<Func>);
    using return_type = decltype(func(std::forward<Args>(args)...));
    using futurator = futurize<return_type>;
    if (sg.active()) {
        return futurator::invoke(func, std::forward<Args>(args)...);
    } else {
        return internal::schedule_in_group(sg, [func = std::move(func), args = std::make_tuple(std::forward<Args>(args)...)] () mutable {
            return futurator::apply(func, std::move(args));
        });
    }
}

/// @}

} // namespace seastar
