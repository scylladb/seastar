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

#include <seastar/core/sstring.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/http/internal/content_source.hh>
#include <seastar/testing/test_case.hh>
#include <tuple>

using namespace seastar;

class buf_source_impl : public data_source_impl {
    temporary_buffer<char> _tmp;
public:
    buf_source_impl(sstring str) : _tmp(str.c_str(), str.size()) {};
    virtual future<temporary_buffer<char>> get() override {
        if (_tmp.empty()) {
            return make_ready_future<temporary_buffer<char>>();
        }
        return make_ready_future<temporary_buffer<char>>(std::move(_tmp));
    }
    virtual future<temporary_buffer<char>> skip(uint64_t n) override {
        _tmp.trim_front(std::min(_tmp.size(), n));
        return make_ready_future<temporary_buffer<char>>();
    }
};

SEASTAR_TEST_CASE(test_incomplete_content) {
    return seastar::async([] {
        auto inp = input_stream<char>(data_source(std::make_unique<buf_source_impl>(sstring("asdfghjkl;"))));
        auto content_strm = input_stream<char>(data_source(std::make_unique<httpd::internal::content_length_source_impl>(inp, 20)));

        auto content1 = content_strm.read().get0();
        BOOST_REQUIRE(temporary_buffer<char>("asdfghjkl;", 10) == content1);
        auto content2 = content_strm.read().get0();
        BOOST_REQUIRE(temporary_buffer<char>() == content2);
        BOOST_REQUIRE(content_strm.eof());
        BOOST_REQUIRE(inp.eof());
    });
}

SEASTAR_TEST_CASE(test_complete_content) {
    return seastar::async([] {
        auto inp = input_stream<char>(data_source(std::make_unique<buf_source_impl>(sstring("asdfghjkl;1234567890"))));
        auto content_strm = input_stream<char>(data_source(std::make_unique<httpd::internal::content_length_source_impl>(inp, 20)));

        auto content1 = content_strm.read().get0();
        BOOST_REQUIRE(temporary_buffer<char>("asdfghjkl;1234567890", 20) == content1);
        auto content2 = content_strm.read().get0();
        BOOST_REQUIRE(temporary_buffer<char>() == content2);
        BOOST_REQUIRE(content_strm.eof());
    });
}

SEASTAR_TEST_CASE(test_more_than_requests_content) {
    return seastar::async([] {
        auto inp = input_stream<char>(data_source(std::make_unique<buf_source_impl>(sstring("asdfghjkl;1234567890xyz"))));
        auto content_strm = input_stream<char>(data_source(std::make_unique<httpd::internal::content_length_source_impl>(inp, 20)));

        auto content1 = content_strm.read().get0();
        BOOST_REQUIRE(temporary_buffer<char>("asdfghjkl;1234567890", 20) == content1);
        auto content2 = content_strm.read().get0();
        BOOST_REQUIRE(temporary_buffer<char>() == content2);
        BOOST_REQUIRE(content_strm.eof());
        auto content3 = inp.read().get0();
        BOOST_REQUIRE(temporary_buffer<char>("xyz", 3) == content3);
    });
}