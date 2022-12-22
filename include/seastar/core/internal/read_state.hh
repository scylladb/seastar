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

#include <cstring>
#include <seastar/core/align.hh>
#include <seastar/core/internal/io_intent.hh>
#include <seastar/core/temporary_buffer.hh>

namespace seastar {
namespace internal {

template <typename CharType>
struct file_read_state {
    typedef temporary_buffer<CharType> tmp_buf_type;

    file_read_state(uint64_t offset, uint64_t front, size_t to_read,
            size_t memory_alignment, size_t disk_alignment, io_intent* intent)
    : buf(tmp_buf_type::aligned(memory_alignment,
                                align_up(to_read, disk_alignment)))
    , _offset(offset)
    , _to_read(to_read)
    , _front(front)
    , _iref(intent) {}

    bool done() const {
        return eof || pos >= _to_read;
    }

    /**
     * Trim the buffer to the actual number of read bytes and cut the
     * bytes from offset 0 till "_front".
     *
     * @note this function has to be called only if we read bytes beyond
     *       "_front".
     */
    void trim_buf_before_ret() {
        if (have_good_bytes()) {
            buf.trim(pos);
            buf.trim_front(_front);
        } else {
            buf.trim(0);
        }
    }

    uint64_t cur_offset() const {
        return _offset + pos;
    }

    size_t left_space() const {
        return buf.size() - pos;
    }

    size_t left_to_read() const {
        // positive as long as (done() == false)
        return _to_read - pos;
    }

    void append_new_data(tmp_buf_type& new_data) {
        auto to_copy = std::min(left_space(), new_data.size());

        std::memcpy(buf.get_write() + pos, new_data.get(), to_copy);
        pos += to_copy;
    }

    bool have_good_bytes() const {
        return pos > _front;
    }

    io_intent* get_intent() {
        return _iref.retrieve();
    }

public:
    bool         eof      = false;
    tmp_buf_type buf;
    size_t       pos      = 0;
private:
    uint64_t     _offset;
    size_t       _to_read;
    uint64_t     _front;
    internal::intent_reference _iref;
};

} // namespace internal
} // namespace seastar
