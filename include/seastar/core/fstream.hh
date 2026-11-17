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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once

/// \file

// File <-> streams adapters
//
// Seastar files are block-based due to the reliance on DMA - you must read
// on sector boundaries.  The adapters in this file provide a byte stream
// interface to files, while retaining the zero-copy characteristics of
// seastar files.
#include <seastar/core/file.hh>
#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/internal/api-level.hh>

#include <cstdint>
#include <filesystem>

namespace seastar {


class file_input_stream_history {
    static constexpr uint64_t window_size = 4 * 1024 * 1024;
    struct window {
        uint64_t total_read = 0;
        uint64_t unused_read = 0;
    };
    window current_window;
    window previous_window;
    unsigned read_ahead = 1;

    friend class file_data_source_impl;
};

/// Data structure describing options for opening a file input stream
struct file_input_stream_options {
    size_t buffer_size = 8192;    ///< I/O buffer size
    unsigned read_ahead = 0;      ///< Maximum number of extra read-ahead operations
    lw_shared_ptr<file_input_stream_history> dynamic_adjustments = { }; ///< Input stream history, if null dynamic adjustments are disabled
};

/// \brief Creates an input_stream to read a portion of a file.
///
/// \param file File to read; multiple streams for the same file may coexist
/// \param offset Starting offset to read from (no alignment restrictions)
/// \param len Maximum number of bytes to read; the stream will stop at end-of-file
///            even if `offset + len` is beyond end-of-file.
/// \param options A set of options controlling the stream.
///
/// \note Multiple input streams may exist concurrently for the same file.
input_stream<char> make_file_input_stream(
        file file, uint64_t offset, uint64_t len, file_input_stream_options options = {});

/// \brief Create an input_stream for a given file, reading starting at a given
///        position of the given file, with the specified options.
/// \param file File to read; multiple streams for the same file may coexist
/// \param offset Starting offset to read from (no alignment restrictions)
///
/// \note Multiple fibers of execution (continuations) may safely open
///       multiple input streams concurrently for the same file.
input_stream<char> make_file_input_stream(
        file file, uint64_t offset, file_input_stream_options = {});

/// Create an input_stream for a given file, with the specified options
/// \param file File to read; multiple streams for the same file may coexist
///
/// \note Multiple fibers of execution (continuations) may safely open
///       multiple input streams concurrently for the same file.
input_stream<char> make_file_input_stream(
        file file, file_input_stream_options = {});

/// Create a data_source for reading the given offset:len range from the file
data_source make_file_data_source(file, uint64_t offset, uint64_t len, file_input_stream_options);

/// Create a data_source for reading the whole file from start to end
data_source make_file_data_source(file, file_input_stream_options);

struct file_output_stream_options {
    // For small files, setting preallocation_size can make it impossible for XFS to find
    // an aligned extent. On the other hand, without it, XFS will divide the file into
    // file_size/buffer_size extents. To avoid fragmentation, we set the default buffer_size
    // to 64k (so each extent will be a minimum of 64k) and preallocation_size to 0 (to avoid
    // extent allocation problems).
    //
    // Large files should increase both buffer_size and preallocation_size.
    unsigned buffer_size = 65536;
    unsigned preallocation_size = 0; ///< Preallocate extents. For large files, set to a large number (a few megabytes) to reduce fragmentation
    unsigned write_behind = 1; ///< Number of buffers to write in parallel
};

/// Create an output_stream for writing starting at the position zero of a
/// newly created file.
/// The file object should be moved into this function because the `output_stream` takes ownership
/// of the file: the file object will be closed when closing the `output_stream`.
///
/// In case stream creation fails, the file will be closed and an exceptional future is returned.
future<output_stream<char>> make_file_output_stream(
        file file,
        uint64_t buffer_size = 8192) noexcept;

/// Create an output_stream for writing starting at the position zero of a
/// newly created file.
/// The file object should be moved into this function because the `output_stream` takes ownership
/// of the file: the file object will be closed when closing the `output_stream`.
///
/// In case stream creation fails, the file will be closed and an exceptional future is returned.
future<output_stream<char>> make_file_output_stream(
        file file,
        file_output_stream_options options) noexcept;

/// Create a data_sink for writing starting at the position zero of a
/// newly created file.
/// Closes the file if the sink creation fails.
future<data_sink> make_file_data_sink(file, file_output_stream_options) noexcept;

/// \defgroup pipe-streams Pipe / stream-fd streams
///
/// These factories create reactor-integrated byte streams backed by any
/// stream-oriented file descriptor: anonymous pipes, named FIFOs, character
/// devices, PTYs, or sockets.  I/O is performed non-blocking via the reactor
/// (poll + read/writev), making them suitable for any fd that supports plain
/// read(2)/write(2) but not seekable or O_DIRECT I/O.
///
/// Two construction modes are available:
///  - From a \c file_desc (takes ownership of the fd).
///  - From a filesystem path (opens the device asynchronously via the thread
///    pool with O_NONBLOCK).
/// @{

/// Create an input_stream that reads from \p fd.
/// The stream takes ownership of \p fd.
input_stream<char> make_pipe_input_stream(file_desc fd, size_t buffer_size = 8192);

/// Create an output_stream that writes to \p fd.
/// The stream takes ownership of \p fd.
output_stream<char> make_pipe_output_stream(file_desc fd, size_t buffer_size = 8192);

/// Asynchronously open the path \p path for reading and return an
/// input_stream backed by it.
future<input_stream<char>> make_pipe_input_stream(std::filesystem::path path, size_t buffer_size = 8192);

/// Asynchronously open the path \p path for writing and return an
/// output_stream backed by it.
future<output_stream<char>> make_pipe_output_stream(std::filesystem::path path, size_t buffer_size = 8192);

/// @}


}
