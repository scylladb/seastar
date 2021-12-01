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

#include <seastar/util/std-compat.hh>

namespace seastar {

class logger;

/// Controls whether on_internal_error() aborts or throws. The default
/// is to throw.
/// \returns the current abort_on_internal_error state.
bool set_abort_on_internal_error(bool do_abort) noexcept;

/// Report an internal error
///
/// Depending on the value passed to set_abort_on_internal_error, this
/// will either log to \p logger and abort or throw a std::runtime_error.
[[noreturn]] void on_internal_error(logger& logger, std::string_view reason);

/// Report an internal error
///
/// Depending on the value passed to set_abort_on_internal_error, this
/// will either log to \p logger and abort or throw the passed-in
/// \p ex.
/// This overload cannot attach a backtrace to the exception, so if the
/// caller wishes to have one attached they have to do it themselves.
[[noreturn]] void on_internal_error(logger& logger, std::exception_ptr ex);

/// Report an internal error in a noexcept context
///
/// The error will be logged to \logger and if set_abort_on_internal_error,
/// was set to true, the program will be aborted. This overload can be used
/// in noexcept contexts like destructors or noexcept functions.
void on_internal_error_noexcept(logger& logger, std::string_view reason) noexcept;

}
