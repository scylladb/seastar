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
 * Copyright (C) 2024 ScyllaDB
 */

#pragma once

#include <type_traits>
#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/util/modules.hh>

namespace seastar {
namespace util {
SEASTAR_MODULE_EXPORT_BEGIN

/// Writes data to stream and properly flushes and closes one
///
/// \param out \ref output_stream to write to
/// \param writer callable invoked to write data itself
///
/// The helper takes ownership of the output_stream, flushes and closes it
/// after the writer is done. The stream is not useable after it resolves.
template <typename W>
requires std::is_invocable_r_v<future<>, W, output_stream<char>&>
future<> write_to_stream_and_close(output_stream<char>&& out_, W writer) {
    output_stream<char> out = std::move(out_);
    std::exception_ptr ex;
    try {
        co_await writer(out);
        co_await out.flush();
    } catch (...) {
        ex = std::current_exception();
    }
    co_await out.close();
    if (ex) {
        co_await coroutine::return_exception_ptr(std::move(ex));
    }
}

SEASTAR_MODULE_EXPORT_END
} // util namespace
} // seastar namespace

