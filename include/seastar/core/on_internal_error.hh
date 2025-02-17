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

#include <seastar/util/assert.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/modules.hh>
#ifndef SEASTAR_MODULE
#include <exception>
#include <string_view>
#endif

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

class logger;

/// Controls whether on_internal_error() aborts or throws. The default
/// is to throw.
/// \returns the current abort_on_internal_error state.
bool set_abort_on_internal_error(bool do_abort) noexcept;

/// Report an internal error
///
/// Depending on the value passed to set_abort_on_internal_error, this
/// will either abort or throw a std::runtime_error.
/// In both cases an error will be logged with \p logger, containing
/// \p reason and the current backtrace.
[[noreturn]] void on_internal_error(logger& logger, std::string_view reason);

/// Report an internal error
///
/// Depending on the value passed to set_abort_on_internal_error, this
/// will either abort or throw the passed-in \p ex.
/// In both cases an error will be logged with \p logger, containing
/// \p ex and the current backtrace.
/// This overload cannot attach a backtrace to the exception, so if the
/// caller wishes to have one attached they have to do it themselves.
[[noreturn]] void on_internal_error(logger& logger, std::exception_ptr ex);

/// Report an internal error in a noexcept context
///
/// The error will be logged to \logger and if set_abort_on_internal_error,
/// was set to true, the program will be aborted. This overload can be used
/// in noexcept contexts like destructors or noexcept functions.
void on_internal_error_noexcept(logger& logger, std::string_view reason) noexcept;

/// Report an internal error and abort unconditionally
///
/// The error will be logged to \logger and the program will be aborted,
/// regardless of the abort_on_internal_error setting.
/// This overload can be used to replace SEASTAR_ASSERT().
[[noreturn]] void on_fatal_internal_error(logger& logger, std::string_view reason) noexcept;

SEASTAR_MODULE_EXPORT_END
}
