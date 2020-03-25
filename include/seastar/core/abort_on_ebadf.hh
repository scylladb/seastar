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
 * Copyright 2019 ScyllaDB
 */

#pragma once

namespace seastar {

/// Determines whether seastar should throw or abort when operation made by
/// seastar fails because the target file descriptor is not valid. This is
/// detected when underlying system calls return EBADF or ENOTSOCK.
/// The default behavior is to throw std::system_error.
void set_abort_on_ebadf(bool do_abort);

/// Queries the current setting for seastar's behavior on invalid file descriptor access.
/// See set_abort_on_ebadf().
bool is_abort_on_ebadf_enabled();

}
