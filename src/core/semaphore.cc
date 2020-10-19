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
 * Copyright (C) 2020 Cloudius Systems, Ltd.
 */

#include <fmt/format.h>
#include <seastar/core/semaphore.hh>

namespace seastar {

// Exception Factory for standard semaphore
//
// constructs standard semaphore exceptions
// \see semaphore_timed_out and broken_semaphore

const char* broken_semaphore::what() const noexcept {
    return "Semaphore broken";
}

const char* semaphore_timed_out::what() const noexcept {
    return "Semaphore timedout";
}

semaphore_timed_out semaphore_default_exception_factory::timeout() {
    return semaphore_timed_out();
}

broken_semaphore semaphore_default_exception_factory::broken() {
    return broken_semaphore();
}

// A factory of semaphore exceptions that contain additional context: the semaphore name
// auto sem = named_semaphore(0, named_semaphore_exception_factory{"file_opening_limit_semaphore"});

named_semaphore_timed_out::named_semaphore_timed_out(std::string_view msg) : _msg(format("Semaphore timed out: {}", msg)) {
}

broken_named_semaphore::broken_named_semaphore(std::string_view msg) : _msg(format("Semaphore broken: {}", msg)) {
}

const char* named_semaphore_timed_out::what() const noexcept {
    return _msg.c_str();
}

const char* broken_named_semaphore::what() const noexcept {
    return _msg.c_str();
}

named_semaphore_timed_out named_semaphore_exception_factory::timeout() {
    return named_semaphore_timed_out(name);
}

broken_named_semaphore named_semaphore_exception_factory::broken() {
    return broken_named_semaphore(name);
}

} // namespace seastar
