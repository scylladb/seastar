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
 * Copyright (C) 2025-present ScyllaDB
 */

#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/util/noncopyable_function.hh>

namespace seastar::http {

/// body_writer_type - a function that accepts an output stream and uses that stream to write the body.
/// The function should take ownership of the stream while using it and must close the stream when done.
using body_writer_type = noncopyable_function<future<>(output_stream<char>&&)>;

} // namespace seastar::http
