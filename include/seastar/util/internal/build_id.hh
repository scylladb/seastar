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
 * Copyright (C) 2019-present ScyllaDB
 */

#pragma once

namespace seastar::internal {

/// Returns the build ID of the currently running executable, if available.
/// The build ID is a unique identifier for a specific build of the executable,
/// often used for debugging and profiling purposes.
/// If the build ID is not available, this function returns "unknown".
const char* get_build_id();

} // namespace seastar::internal
