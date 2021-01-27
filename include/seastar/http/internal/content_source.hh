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

#include <seastar/core/iostream.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/http/common.hh>
#include <seastar/util/log.hh>

namespace seastar {

namespace httpd {

namespace internal {

/*
 * An input_stream wrapper that allows to read only "length" bytes
 * from it, used to handle requests with large bodies.
 * */
class content_length_source_impl : public data_source_impl {
    input_stream<char>& _inp;
    size_t _remaining_bytes = 0;
public:
    content_length_source_impl(input_stream<char>& inp, size_t length)
        : _inp(inp), _remaining_bytes(length) {
    }

    virtual future<temporary_buffer<char>> get() override {
        if (_remaining_bytes == 0) {
            return make_ready_future<temporary_buffer<char>>();
        }
        return _inp.read_up_to(_remaining_bytes).then([this] (temporary_buffer<char> tmp_buf) {
            _remaining_bytes -= tmp_buf.size();
            return tmp_buf;
        });
    }

    virtual future<temporary_buffer<char>> skip(uint64_t n) override {
        uint64_t skip_bytes = std::min(n, _remaining_bytes);
        _remaining_bytes -= skip_bytes;
        return _inp.skip(skip_bytes).then([this] {
            return temporary_buffer<char>();
        });
    }

    virtual future<> close() override {
        return make_ready_future<>();
    }
};

} // namespace internal

} // namespace httpd

}
