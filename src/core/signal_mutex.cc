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

#include <seastar/core/internal/signal_mutex.hh>

namespace seastar::internal {

signal_mutex::guard::~guard() {
    if (_mutex == nullptr) {
        return;
    }
    // Ensure the subsequent store isn't hoisted by the the
    // compiler into the critical section it's intended to
    // protect.
    std::atomic_signal_fence(std::memory_order_release);
    _mutex->_mutex.store(false, std::memory_order_relaxed);
}

std::optional<signal_mutex::guard> signal_mutex::try_lock() {
    if (!_mutex.load(std::memory_order_relaxed)) {
        _mutex.store(true, std::memory_order_relaxed);
        // Ensure that this read-modify-update operation isn't
        // mixed into the critical section it's intended to protect
        // by the compiler.
        std::atomic_signal_fence(std::memory_order_acq_rel);
        return {guard(this)};
    }

    return std::nullopt;
}

} // namespace seastar::internal
