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
 * Copyright (C) 2021 ScyllaDB
 */

#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/temporary_buffer.hh>

namespace seastar {

namespace util {

future<std::vector<temporary_buffer<char>>> read_entire_stream(input_stream<char>& inp) {
    using tmp_buf = temporary_buffer<char>;
    using consumption_result_type = consumption_result<char>;
    return do_with(std::vector<tmp_buf>(), [&inp] (std::vector<tmp_buf>& bufs) {
        return inp.consume([&bufs] (tmp_buf buf) {
            if (buf.empty()) {
                return make_ready_future<consumption_result_type>(stop_consuming(std::move(buf)));
            }
            bufs.push_back(std::move(buf));
            return make_ready_future<consumption_result_type>(continue_consuming());
        }).then([&bufs] {
            return std::move(bufs);
        });
    });
}

future<sstring> read_entire_stream_contiguous(input_stream<char>& inp) {
    return read_entire_stream(inp).then([] (std::vector<temporary_buffer<char>> bufs) {
        size_t total_size = 0;
        for (auto&& buf : bufs) {
            total_size += buf.size();
        }
        sstring ret(sstring::initialized_later(), total_size);
        size_t pos = 0;
        for (auto&& buf : bufs) {
            std::copy(buf.begin(), buf.end(), ret.data() + pos);
            pos += buf.size();
        }
        return ret;
    });
};

future<> skip_entire_stream(input_stream<char>& inp) {
    return inp.consume([] (temporary_buffer<char> tmp) {
        return tmp.empty() ? make_ready_future<consumption_result<char>>(stop_consuming(temporary_buffer<char>()))
                           : make_ready_future<consumption_result<char>>(continue_consuming());
    });
}

}

}