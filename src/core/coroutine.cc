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
 * Copyright (C) 2026 ScyllaDB
 */

#ifdef SEASTAR_MODULE
module;
module seastar;
#else
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/at_coroutine_exit.hh>
#endif

namespace seastar::internal {

// HEAD of linked list of coroutine promises making up the coroutine call stack.
thread_local coroutine_promise_base* current_coroutine_promise = nullptr;

void coroutine_promise_base::push_coroutine() {
    internal::current_coroutine_promise = this;
}

void coroutine_promise_base::pop_coroutine() {
    internal::current_coroutine_promise = nullptr;
}

} // namespace seastar::internal

namespace seastar::coroutine {

void at_coroutine_exit(noncopyable_function<future<>(std::exception_ptr)> func) {
    if (!internal::current_coroutine_promise) {
        throw std::logic_error("coroutine::at_coroutine_exit() can only be called from a coroutine");
    }

    internal::current_coroutine_promise->push_at_exit_function(std::move(func));
}

} // namespace seastar::coroutine
