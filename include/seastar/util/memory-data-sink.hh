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
 * Copyright (C) 2025 ScyllaDB
 */

#pragma once

#include <algorithm>
#include <seastar/core/iostream.hh>

namespace seastar {
namespace util {

#if SEASTAR_API_LEVEL >= 9

template <template <typename T> class Container>
requires requires (Container<temporary_buffer<char>>& col, size_t s) {
    typename std::back_insert_iterator<Container<temporary_buffer<char>>>;
}
inline void append_buffers(Container<temporary_buffer<char>>& c, std::span<temporary_buffer<char>> bufs) {
    std::ranges::move(bufs, std::back_inserter(c));
}

template <typename Container, size_t DefaultBufferSize = 1024>
requires requires (Container& ct, std::span<temporary_buffer<char>> bufs) {
    { append_buffers(ct, bufs) };
}
class basic_memory_data_sink final : public data_sink_impl {
    Container& _container;
    const size_t _buffer_size;

public:
    basic_memory_data_sink(Container& ct, size_t buffer_size = DefaultBufferSize)
        : _container(ct)
        , _buffer_size(buffer_size)
    {
    }

    virtual future<> put(std::span<temporary_buffer<char>> bufs) override {
        append_buffers(this->_container, bufs);
        return make_ready_future<>();
    }

    virtual future<> flush() override {
        return make_ready_future<>();
    }

    virtual future<> close() override {
        return make_ready_future<>();
    }

    virtual size_t buffer_size() const noexcept override {
        return _buffer_size;
    }
};

using memory_data_sink = basic_memory_data_sink<std::vector<temporary_buffer<char>>>;

#endif

}
}
