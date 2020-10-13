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

private:
    void free_buffer() noexcept;
    void realloc_buffer();

public:
    class inserter_iterator {
    public:
        using iterator_category = std::output_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = char;
        using pointer = char*;
        using reference = char&;

    private:
        log_buf* _buf;
        char* _current;

    public:
        explicit inserter_iterator(log_buf& buf) noexcept : _buf(&buf), _current(_buf->_current) { }
        inserter_iterator(const inserter_iterator& o) noexcept : _buf(o._buf), _current(o._current) { }

        reference operator*() {
            if (__builtin_expect(_current == _buf->_end, false)) {
                _buf->realloc_buffer();
                _current = _buf->_current;
            }
            return *_current;
        }
        inserter_iterator& operator++() noexcept {
            if (__builtin_expect(_current == _buf->_current, true)) {
                ++_buf->_current;
            }
            ++_current;
            return *this;
        }
        inserter_iterator operator++(int) noexcept {
            inserter_iterator o(*this);
            ++(*this);
            return o;
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
    /// Create an output iterator which allows writing into the buffer.
    inserter_iterator back_insert_begin() noexcept { return inserter_iterator(*this); }
    /// The amount of data written so far.
    const size_t size() const noexcept { return _current - _begin; }
    /// The size of the buffer.
    const size_t capacity() const noexcept { return _end - _begin; }
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
