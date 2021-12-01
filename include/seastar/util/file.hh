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

#include <seastar/core/future.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/sstring.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/short_streams.hh>

namespace seastar {

/// Recursively removes a directory and all of its contents.
///
/// \param path path of the directory to recursively remove
///
/// \note
/// Unlike `rm -rf` path has to be a directory and may not refer to a regular file.
///
/// The function flushes the parent directory of the removed path and so guaranteeing that
/// the remove is stable on disk.
///
/// The function bails out on first error. In that case, some files and/or sub-directories
/// (and their contents) may be left behind at the level in which the error was detected.
///
future<> recursive_remove_directory(std::filesystem::path path) noexcept;

/// @}

/// \defgroup fileio-util File and Stream Utilities
/// \ingroup fileio-module
///
/// \brief
/// These utilities are provided to help perform operations on files and I/O streams.

namespace util {

/// \addtogroup fileio-util
/// @{

template <typename Func>
SEASTAR_CONCEPT(requires requires(Func func, input_stream<char>& in) {
     { func(in) };
})
auto with_file_input_stream(const std::filesystem::path& path, Func func, file_open_options file_opts = {}, file_input_stream_options input_stream_opts = {}) {
    static_assert(std::is_nothrow_move_constructible_v<Func>);
    return open_file_dma(path.native(), open_flags::ro, std::move(file_opts)).then(
            [func = std::move(func), input_stream_opts = std::move(input_stream_opts)] (file f) mutable {
        return do_with(make_file_input_stream(std::move(f), std::move(input_stream_opts)),
                [func = std::move(func)] (input_stream<char>& in) mutable {
            return futurize_invoke(std::move(func), in).finally([&in] {
                return in.close();
            });
        });
    });
}

/// Returns all bytes from the file until eof, accessible in chunks.
///
/// \note use only on short files to avoid running out of memory.
///
/// \param path path of the file to be read.
future<std::vector<temporary_buffer<char>>> read_entire_file(std::filesystem::path path);

/// Returns all bytes from the file until eof as a single buffer.
///
/// \note use only on short files to avoid running out of memory.
///
/// \param path path of the file to be read.
future<sstring> read_entire_file_contiguous(std::filesystem::path path);

/// @}

} // namespace util

} // namespace seastar
