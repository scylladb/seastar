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

#include <seastar/core/ragel.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/http/request.hh>
#include <seastar/http/request_parser.hh>
#include <seastar/testing/test_case.hh>
#include <tuple>
#include <utility>
#include <vector>

using namespace seastar;

SEASTAR_TEST_CASE(test_header_parsing) {
    struct test_set {
        sstring msg;
        bool parsable;
        sstring header_name = "";
        sstring header_value = "";

        temporary_buffer<char> buf() {
            return temporary_buffer<char>(msg.c_str(), msg.size());
        }
    };

    std::vector<test_set> tests = {
        { "GET /test HTTP/1.1\r\nHost: test\r\n\r\n", true, "Host", "test" },
        { "GET /hello HTTP/1.0\r\nHeader: Field\r\n\r\n", true, "Header", "Field" },
        { "GET /hello HTTP/1.0\r\nHeader: \r\n\r\n", true, "Header", "" },
        { "GET /hello HTTP/1.0\r\nHeader:  f  i e l d  \r\n\r\n", true, "Header", "f  i e l d" },
        { "GET /hello HTTP/1.0\r\nHeader: fiel\r\n    d\r\n\r\n", true, "Header", "fiel d" },
        { "GET /hello HTTP/1.0\r\ntchars.^_`|123: printable!@#%^&*()obs_text\x80\x81\xff\r\n\r\n", true,
            "tchars.^_`|123", "printable!@#%^&*()obs_text\x80\x81\xff" },
        { "GET /hello HTTP/1.0\r\nHeader: Field\r\nHeader: Field2\r\n\r\n", true, "Header", "Field,Field2" },
        { "GET /hello HTTP/1.0\r\n\r\n", true },
        { "GET /hello HTTP/1.0\r\nHeader : Field\r\n\r\n", false },
        { "GET /hello HTTP/1.0\r\nHeader Field\r\n\r\n", false },
        { "GET /hello HTTP/1.0\r\nHeader@: Field\r\n\r\n", false },
        { "GET /hello HTTP/1.0\r\nHeader: fiel\r\nd \r\n\r\n", false }
    };

    http_request_parser parser;
    for (auto& tset : tests) {
        parser.init();
        BOOST_REQUIRE(parser(tset.buf()).get().has_value());
        BOOST_REQUIRE_NE(parser.failed(), tset.parsable);
        if (tset.parsable) {
            auto req = parser.get_parsed_request();
            BOOST_REQUIRE_EQUAL(req->get_header(std::move(tset.header_name)), std::move(tset.header_value));
        }
    }
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_request_size_limit) {
    struct test_set {
        sstring msg;
        size_t limit;
        bool too_large;

        temporary_buffer<char> buf() {
            return temporary_buffer<char>(msg.c_str(), msg.size());
        }
    };

    sstring big_uri = "GET /" + sstring(1024, 'a') + " HTTP/1.1\r\nHost: test\r\n\r\n";
    sstring big_header = "GET /test HTTP/1.1\r\nX-Big: " + sstring(1024, 'a') + "\r\n\r\n";

    std::vector<test_set> tests = {
        { "GET /test HTTP/1.1\r\nHost: test\r\n\r\n", 64, false },
        { big_uri, 64, true },
        { big_header, 64, true },
        { big_uri, 4096, false },
    };

    http_request_parser parser;
    for (auto& tset : tests) {
        parser.init();
        parser.set_size_limit(tset.limit);
        BOOST_REQUIRE(parser(tset.buf()).get().has_value());
        BOOST_REQUIRE_EQUAL(parser.size_limit_exceeded(), tset.too_large);
    }
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_request_size_limit_scattered) {
    // The limit must hold when a single oversized header is split across
    // buffers, since the parser accumulates it before the request completes.
    sstring part1 = "GET /test HTTP/1.1\r\nX-Big: " + sstring(200, 'a');
    sstring part2 = sstring(400, 'a') + "\r\n\r\n";
    auto feed = [] (http_request_parser& parser, const sstring& s) {
        return parser(temporary_buffer<char>(s.c_str(), s.size())).get();
    };

    http_request_parser parser;
    parser.init();
    parser.set_size_limit(512);
    BOOST_REQUIRE(!feed(parser, part1).has_value());
    BOOST_REQUIRE(!parser.size_limit_exceeded());
    feed(parser, part2);
    BOOST_REQUIRE(parser.size_limit_exceeded());
    return make_ready_future<>();
}
