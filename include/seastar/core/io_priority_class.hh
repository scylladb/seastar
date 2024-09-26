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
 * Copyright 2021 ScyllaDB
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <seastar/core/sstring.hh>
#include <seastar/core/future.hh>
#include <seastar/util/modules.hh>

#endif

namespace seastar {

class io_queue;

SEASTAR_MODULE_EXPORT_BEGIN

using io_priority_class_id = unsigned;

SEASTAR_MODULE_EXPORT_END

namespace internal {
struct maybe_priority_class_ref {
};
}

} // namespace seastar
