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
#include <seastar/core/thread.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/http/chunk_parsers.hh>
#include <seastar/http/internal/content_source.hh>
#include <seastar/testing/test_case.hh>
#include <tuple>
#include <utility>
#include <vector>

using namespace seastar;

SEASTAR_TEST_CASE(test_size_and_extensions_parsing) {
    struct test_set {
        sstring msg;
        bool parsable;
        sstring size = "";
        std::vector<std::pair<sstring, sstring>> extensions;

        temporary_buffer<char> buf() {
            return temporary_buffer<char>(msg.c_str(), msg.size());
        }
    };

    std::vector<test_set> tests = {
        { "14;name=value\r\n", true, "14", { {"name", "value"} } },
        { "abcdef;name=value;name2=\"value2\"\r\n", true, "abcdef" },
        { "1efg;name=value\r\n", false },
        { "aa;tchars.^_`|123=t1!#$%&'*+-.~\r\n", true, "aa", { {"tchars.^_`|123", "t1!#$%&'*+-.~"} } },
        { "1;quoted=\"hello world\";quoted-pair=\"\\a\\b\\cd\\\\ef\"\r\n", true, "1", { {"quoted", "hello world"}, {"quoted-pair", "abcd\\ef"} } },
        { "2;bad-quoted-pair=\"abc\\\"\r\n", false },
        { "3;quoted-pair-outside-quoted-string=\\q\\p\r\n", false },
        { "4;whitespace-outside-quoted-string=quoted string\r\n", false },
        { "5;quotation-mark-inside-quoted-string=\"quoted\"mark\"\r\n", false },
        { "6; bad=space\r\n", false },
        { "7;sole-name\r\n", true, "7", { { "sole-name", ""} } },
        { "8;empty_value=\"\"\r\n", true, "8", { { "empty_value", ""} } },
        { "0\r\n", true, "0" }
    };

    http_chunk_size_and_ext_parser parser;
    for (auto& tset : tests) {
        parser.init();
        BOOST_REQUIRE(parser(tset.buf()).get0().has_value());
        BOOST_REQUIRE_NE(parser.failed(), tset.parsable);
        if (tset.parsable) {
            BOOST_REQUIRE_EQUAL(parser.get_size(), std::move(tset.size));
            auto exts = parser.get_parsed_extensions();
            for (auto& ext : tset.extensions) {
                BOOST_REQUIRE_EQUAL(exts[ext.first], ext.second);
            }
        }
    }
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_trailer_headers_parsing) {
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
        // the headers follow the same rules as in the request parser
        { "Host: test\r\n\r\n", true, "Host", "test" },
        { "Header: Field\r\n\r\n", true, "Header", "Field" },
        { "Header: \r\n\r\n", true, "Header", "" },
        { "Header:  f  i e l d  \r\n\r\n", true, "Header", "f  i e l d" },
        { "Header: fiel\r\n    d\r\n\r\n", true, "Header", "fiel d" },
        { "tchars.^_`|123: printable!@#%^&*()obs_text\x80\x81\xff\r\n\r\n", true,
            "tchars.^_`|123", "printable!@#%^&*()obs_text\x80\x81\xff" },
        { "Header: Field\r\nHeader: Field2\r\n\r\n", true, "Header", "Field,Field2" },
        { "Header : Field\r\n\r\n", false },
        { "Header Field\r\n\r\n", false },
        { "Header@: Field\r\n\r\n", false },
        { "Header: fiel\r\nd \r\n\r\n", false },
        { "\r\n", true }
    };

    http_chunk_trailer_parser parser;
    for (auto& tset : tests) {
        parser.init();
        BOOST_REQUIRE(parser(tset.buf()).get0().has_value());
        BOOST_REQUIRE_NE(parser.failed(), tset.parsable);
        if (tset.parsable) {
            auto heads = parser.get_parsed_headers();
            BOOST_REQUIRE_EQUAL(heads[std::move(tset.header_name)], std::move(tset.header_value));
        }
    }
    return make_ready_future<>();
}
