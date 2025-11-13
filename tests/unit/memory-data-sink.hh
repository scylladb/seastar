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
 * Copyright 2025 ScyllaDB
 */

#pragma once

namespace seastar {
namespace testing {

/*!
 * \brief a helper data sink that stores everything it gets in a stringstream
 */
class memory_data_sink_impl : public data_sink_impl {
    std::stringstream& _ss;
    const size_t _buffer_size;
public:
    static constexpr size_t default_buffer_size = 1024;
    memory_data_sink_impl(std::stringstream& ss, size_t buffer_size = default_buffer_size)
        : _ss(ss)
        , _buffer_size(buffer_size)
    {
    }
#if SEASTAR_API_LEVEL >= 9
    future<> put(std::span<temporary_buffer<char>> bufs) override {
        for (auto& buf : bufs) {
            _ss.write(buf.get(), buf.size());
        }
        return make_ready_future<>();
    }
#else
    virtual future<> put(net::packet data)  override {
        return data_sink_impl::fallback_put(std::move(data));
    }
    virtual future<> put(temporary_buffer<char> buf) override {
        _ss.write(buf.get(), buf.size());
        return make_ready_future<>();
    }
#endif
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

class memory_data_sink : public data_sink {
public:
    memory_data_sink(std::stringstream& ss)
        : data_sink(std::make_unique<memory_data_sink_impl>(ss)) {}
};

} // testing namespace
} // seastar namespace
