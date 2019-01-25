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
 * Copyright 2017 ScyllaDB
 */

#include <seastar/util/alloc_failure_injector.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/util/log.hh>
#include <seastar/util/defer.hh>

namespace seastar {
namespace memory {

static logger log("failure_injector");

thread_local alloc_failure_injector the_alloc_failure_injector;

void alloc_failure_injector::fail() {
    _failed = true;
    cancel();
    if (log.is_enabled(log_level::trace)) {
        log.trace("Failing at {}", current_backtrace());
    }
    _on_alloc_failure();
}

void alloc_failure_injector::run_with_callback(noncopyable_function<void()> callback, noncopyable_function<void()> to_run) {
    auto restore = defer([this, prev = std::exchange(_on_alloc_failure, std::move(callback))] () mutable {
        _on_alloc_failure = std::move(prev);
    });
    to_run();
}

}
}
