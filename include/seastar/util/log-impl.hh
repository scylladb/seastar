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
 * Copyright (C) 2020 Cloudius Systems, Ltd.
 */

#pragma once

#include <iterator>

/// \addtogroup logging
/// @{

namespace seastar {

/// \cond internal
namespace internal {

/// A buffer to format log messages into.
///
/// It was designed to allow formatting the entire message into it, without any
/// intermediary buffers. To minimize the amount of reallocations it supports
/// using an external buffer. When this is full it moves to using buffers
/// allocated by itself.
/// To accommodate the most widely used way of formatting messages -- fmt --,
/// it provides an output iterator interface for writing into it.
class log_buf {
    char* _begin;
    char* _end;
    char* _current;
    bool _own_buf;
    bool _alloc_failure = false;
private:
    void free_buffer() noexcept;
    void realloc_buffer_and_append(char c) noexcept;

public:
    // inserter_iterator is designed like std::back_insert_iterator:
    // operator*, operator++ and operator++(int) are no-ops,
    // and all the work happens in operator=, which pushes a character
    // to the buffer.
    // The iterator stores no state of its own.
    //
    // inserter_iterator is supposed to be used as an output_iterator.
    // That is, assignment is expected to alternate with incrementing.
    class inserter_iterator {
    public:
        using iterator_category = std::output_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = void;
        using pointer = void;
        using reference = void;

    private:
        log_buf* _buf;

    public:
        explicit inserter_iterator(log_buf& buf) noexcept : _buf(&buf) { }

        inserter_iterator& operator=(char c) noexcept {
            if (__builtin_expect(_buf->_current == _buf->_end, false)) {
                _buf->realloc_buffer_and_append(c);
                return *this;
            }
            *_buf->_current++ = c;
            return *this;
        }
        inserter_iterator& operator*() noexcept {
            return *this;
        }
        inserter_iterator& operator++() noexcept {
            return *this;
        }
        inserter_iterator operator++(int) noexcept {
            return *this;
        }
    };

    /// Default ctor.
    ///
    /// Allocates an internal buffer of 512 bytes.
    log_buf();
    /// External buffer ctor.
    ///
    /// Use the external buffer until its full, then switch to internally
    /// allocated buffer. log_buf doesn't take ownership of the buffer.
    log_buf(char* external_buf, size_t size) noexcept;
    ~log_buf();
    /// Clear the buffer, setting its position back to the start, but does not
    /// free any buffers (after this called, size is zero, capacity is unchanged).
    /// Any existing iterators are invalidated.
    void clear() { _current = _begin; }
    /// Create an output iterator which allows writing into the buffer.
    inserter_iterator back_insert_begin() noexcept { return inserter_iterator(*this); }
    /// The amount of data written so far.
    size_t size() const noexcept { return _current - _begin; }
    /// The size of the buffer.
    size_t capacity() const noexcept { return _end - _begin; }
    /// Read only pointer to the buffer.
    /// Note that the buffer is not guaranteed to be null terminated. The writer
    /// has to ensure that, should it wish to.
    const char* data() const noexcept { return _begin; }
    /// A view of the buffer content.
    std::string_view view() const noexcept { return std::string_view(_begin, size()); }
};

} // namespace internal
/// \endcond

} // namespace seastar
