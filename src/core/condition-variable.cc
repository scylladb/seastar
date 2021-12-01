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
 * Copyright (C) 2020 ScyllaDB, Ltd.
 */

#include <seastar/core/condition-variable.hh>

namespace seastar {

const char* broken_condition_variable::what() const noexcept {
    return "Condition variable is broken";
}

const char* condition_variable_timed_out::what() const noexcept {
    return "Condition variable timed out";
}

condition_variable_timed_out condition_variable::condition_variable_exception_factory::timeout() noexcept {
    static_assert(std::is_nothrow_default_constructible_v<condition_variable_timed_out>);
    return condition_variable_timed_out();
}

broken_condition_variable condition_variable::condition_variable_exception_factory::broken() noexcept {
    static_assert(std::is_nothrow_default_constructible_v<broken_condition_variable>);
    return broken_condition_variable();
}

} // namespace seastar
