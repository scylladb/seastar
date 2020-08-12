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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */


/** @file */

#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/do_with.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/noncopyable_function.hh>

#include <seastar/core/with_scheduling_group.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/core/with_timeout.hh>

namespace seastar {

inline
future<> now() {
    return make_ready_future<>();
}

// Returns a future which is not ready but is scheduled to resolve soon.
future<> later() noexcept;

/// @}

} // namespace seastar
