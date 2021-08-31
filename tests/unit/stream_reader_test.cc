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
 * Copyright (C) 2020 ScyllaDB.
 */

#include <seastar/core/future.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/thread.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/http/request.hh>
#include <seastar/util/short_streams.hh>
#include <string>

using namespace seastar;
using namespace util;

/*
 * Simple data source producing up to total_size bytes
 * in buffer_size-byte chunks.
 * */
class test_source_impl : public data_source_impl {
    short _current_letter = 0; // a-z corresponds to 0-25
    size_t _buffer_size;
    size_t _remaining_size;
public:
    test_source_impl(size_t buffer_size, size_t total_size)
        : _buffer_size(buffer_size), _remaining_size(total_size) {
    }
    virtual future<temporary_buffer<char>> get() override {
        size_t len = std::min(_buffer_size, _remaining_size);
        temporary_buffer<char> tmp(len);
        for (size_t i = 0; i < len; i++) {
            tmp.get_write()[i] = 'a' + _current_letter;
            ++_current_letter %= 26;
        }
        _remaining_size -= len;
        return make_ready_future<temporary_buffer<char>>(std::move(tmp));
    }
    virtual future<temporary_buffer<char>> skip(uint64_t n) override {
        _remaining_size -= std::min(_remaining_size, n);
        _current_letter += n %= 26;
        return make_ready_future<temporary_buffer<char>>();
    }
};

SEASTAR_TEST_CASE(test_read_all) {
    return async([] {
        auto check_read_all = [] (input_stream<char>& strm, const char* test) {
            auto all = read_entire_stream(strm).get0();
            sstring s;
            for (auto&& buf: all) {
                s += seastar::to_sstring(std::move(buf));
            };
            BOOST_REQUIRE_EQUAL(s, test);
        };
        input_stream<char> inp(data_source(std::make_unique<test_source_impl>(5, 15)));
        check_read_all(inp, "abcdefghijklmno");
        BOOST_REQUIRE(inp.eof());
        input_stream<char> inp2(data_source(std::make_unique<test_source_impl>(5, 16)));
        check_read_all(inp2, "abcdefghijklmnop");
        BOOST_REQUIRE(inp2.eof());
        input_stream<char> empty_inp(data_source(std::make_unique<test_source_impl>(5, 0)));
        check_read_all(empty_inp, "");
        BOOST_REQUIRE(empty_inp.eof());

        input_stream<char> inp_cont(data_source(std::make_unique<test_source_impl>(5, 15)));
        BOOST_REQUIRE_EQUAL(to_sstring(read_entire_stream_contiguous(inp_cont).get0()), "abcdefghijklmno");
        BOOST_REQUIRE(inp_cont.eof());
        input_stream<char> inp_cont2(data_source(std::make_unique<test_source_impl>(5, 16)));
        BOOST_REQUIRE_EQUAL(to_sstring(read_entire_stream_contiguous(inp_cont2).get0()), "abcdefghijklmnop");
        BOOST_REQUIRE(inp_cont2.eof());
        input_stream<char> empty_inp_cont(data_source(std::make_unique<test_source_impl>(5, 0)));
        BOOST_REQUIRE_EQUAL(to_sstring(read_entire_stream_contiguous(empty_inp_cont).get0()), "");
        BOOST_REQUIRE(empty_inp_cont.eof());
    });
}

SEASTAR_TEST_CASE(test_skip_all) {
    return async([] {
        input_stream<char> inp(data_source(std::make_unique<test_source_impl>(5, 15)));
        skip_entire_stream(inp).get();
        BOOST_REQUIRE(inp.eof());
        BOOST_REQUIRE(to_sstring(inp.read().get0()).empty());
        input_stream<char> inp2(data_source(std::make_unique<test_source_impl>(5, 16)));
        skip_entire_stream(inp2).get();
        BOOST_REQUIRE(inp2.eof());
        BOOST_REQUIRE(to_sstring(inp2.read().get0()).empty());
        input_stream<char> empty_inp(data_source(std::make_unique<test_source_impl>(5, 0)));
        skip_entire_stream(empty_inp).get();
        BOOST_REQUIRE(empty_inp.eof());
        BOOST_REQUIRE(to_sstring(empty_inp.read().get0()).empty());
    });
}
