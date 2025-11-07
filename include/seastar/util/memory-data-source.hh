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

#include <seastar/core/iostream.hh>

namespace seastar {
namespace util {

template <template <typename T> class Container, typename Element>
concept CollectionOf = requires (Container<Element> col) {
    { col.begin() };
    { col.end() };
    { *col.begin() } -> std::same_as<Element&>;
};

template <template <typename T> class Container>
requires (CollectionOf<Container, temporary_buffer<char>> && std::is_nothrow_move_constructible_v<Container<temporary_buffer<char>>>)
class basic_memory_data_source final : public data_source_impl {
    using real_container = Container<temporary_buffer<char>>;
    real_container _bufs;
    real_container::iterator _cur;

public:
    explicit basic_memory_data_source(real_container&& b) noexcept
        : _bufs(std::move(b))
        , _cur(_bufs.begin())
    {}

    virtual future<temporary_buffer<char>> get() override {
        return make_ready_future<temporary_buffer<char>>(_cur != _bufs.end() ? std::move(*_cur++) : temporary_buffer<char>());
    }
};

using memory_data_source = basic_memory_data_source<std::vector>;

template <template <typename T> class C>
requires requires (C<temporary_buffer<char>> t) { basic_memory_data_source(std::move(t)); }
inline input_stream<char> as_input_stream(C<temporary_buffer<char>>&& bufs) {
    return input_stream<char>(data_source(std::make_unique<basic_memory_data_source<C>>(std::move(bufs))));
}

class temporary_buffer_data_source final : public data_source_impl {
    temporary_buffer<char> _buf;

public:
    explicit temporary_buffer_data_source(temporary_buffer<char>&& b) noexcept
        : _buf(std::move(b))
    {}

    virtual future<temporary_buffer<char>> get() override {
        return make_ready_future<temporary_buffer<char>>(std::exchange(_buf, {}));
    }
};

inline input_stream<char> as_input_stream(temporary_buffer<char>&& buf) {
    return input_stream<char>(data_source(std::make_unique<temporary_buffer_data_source>(std::move(buf))));
}

} // util namespace
} // seastar namespace
