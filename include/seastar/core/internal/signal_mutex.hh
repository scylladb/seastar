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
 * Copyright (C) 2025 ScyllaDB
 */

#pragma once

#include <atomic>
#include <optional>

namespace seastar::internal {

/// A lightweight mutex designed to work with interrupts
/// utilizing only compiler barriers.
class signal_mutex {
public:
    class guard {
    private:
        signal_mutex* _mutex;
        guard(signal_mutex* m) : _mutex(m) {}
        friend class signal_mutex;
    public:
        guard(guard&& o) : _mutex(o._mutex) { o._mutex = nullptr; }
        ~guard();
    };

    // Returns a `guard` if the lock was acquired.
    // Otherwise returns a nullopt.
    std::optional<guard> try_lock();

private:
    friend class guard;
    std::atomic_bool _mutex;
};

} // namespace seastar::internal
