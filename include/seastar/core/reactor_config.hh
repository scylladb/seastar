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
 * Copyright (C) 2019 ScyllaDB
 */

#pragma once

#include <chrono>

namespace seastar {

/// Configuration structure for reactor
///
/// This structure provides configuration items for the reactor. It is typically
/// provided by \ref app_template, not the user.
struct reactor_config {
    std::chrono::duration<double> task_quota{0.5e-3}; ///< default time between polls
    /// \brief Handle SIGINT/SIGTERM by calling reactor::stop()
    ///
    /// When true, Seastar will set up signal handlers for SIGINT/SIGTERM that call
    /// reactor::stop(). The reactor will then execute callbacks installed by
    /// reactor::at_exit().
    ///
    /// When false, Seastar will not set up signal handlers for SIGINT/SIGTERM
    /// automatically. The default behavior (terminate the program) will be kept.
    /// You can adjust the behavior of SIGINT/SIGTERM by installing signal handlers
    /// via reactor::handle_signal().
    bool auto_handle_sigint_sigterm = true;  ///< automatically terminate on SIGINT/SIGTERM
};

}
