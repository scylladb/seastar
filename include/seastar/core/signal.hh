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

#pragma once

/// \file

// Seastar interface for POSIX signals.
#include <seastar/util/modules.hh>
#include <seastar/util/noncopyable_function.hh>

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

/// \brief Sets a signal handler for the specified signal.
///
/// \param signo Signal number.
/// \param handler Function to handle the signal.
/// \param once Should the handler be invoked only once.
void handle_signal(int signo, noncopyable_function<void ()>&& handler, bool once = false);

SEASTAR_MODULE_EXPORT_END

}
