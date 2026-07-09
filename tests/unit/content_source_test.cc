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
#include <seastar/http/exception.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/util/memory-data-source.hh>
#include <tuple>

using namespace seastar;

SEASTAR_TEST_CASE(test_incomplete_content) {
    return seastar::async([] {
      {
        auto inp = util::as_input_stream(temporary_buffer<char>::copy_of("asdfghjkl;"));
        auto content_strm = input_stream<char>(data_source(std::make_unique<httpd::internal::content_length_source_impl>(inp, 20)));

        auto content1 = content_strm.read().get();
        BOOST_REQUIRE(temporary_buffer<char>("asdfghjkl;", 10) == content1);
        auto content2 = content_strm.read().get();
        BOOST_REQUIRE(temporary_buffer<char>() == content2);
        BOOST_REQUIRE(content_strm.eof());
        BOOST_REQUIRE(inp.eof());
      }

      {
        auto inp = util::as_input_stream(temporary_buffer<char>::copy_of("4\r\n132"));
        std::unordered_map<sstring, sstring> tmp, tmp2;
        auto content_strm = input_stream<char>(data_source(std::make_unique<httpd::internal::chunked_source_impl>(inp, tmp, tmp2)));

        auto content1 = content_strm.read().get();
        BOOST_REQUIRE(temporary_buffer<char>("132", 3) == content1);
        auto content2 = content_strm.read().get();
        BOOST_REQUIRE(temporary_buffer<char>() == content2);
        BOOST_REQUIRE(content_strm.eof());
      }
    });
}

SEASTAR_TEST_CASE(test_complete_content) {
    return seastar::async([] {
      {
        auto inp = util::as_input_stream(temporary_buffer<char>::copy_of("asdfghjkl;1234567890"));
        auto content_strm = input_stream<char>(data_source(std::make_unique<httpd::internal::content_length_source_impl>(inp, 20)));

        auto content1 = content_strm.read().get();
        BOOST_REQUIRE(temporary_buffer<char>("asdfghjkl;1234567890", 20) == content1);
        auto content2 = content_strm.read().get();
        BOOST_REQUIRE(temporary_buffer<char>() == content2);
        BOOST_REQUIRE(content_strm.eof());
      }

      {
        auto inp = util::as_input_stream(temporary_buffer<char>::copy_of("4\r\n1324\r\n0\r\n\r\n"));
        std::unordered_map<sstring, sstring> tmp, tmp2;
        auto content_strm = input_stream<char>(data_source(std::make_unique<httpd::internal::chunked_source_impl>(inp, tmp, tmp2)));

        auto content1 = content_strm.read().get();
        BOOST_REQUIRE(temporary_buffer<char>("1324", 4) == content1);
        auto content2 = content_strm.read().get();
        BOOST_REQUIRE(temporary_buffer<char>() == content2);
        BOOST_REQUIRE(content_strm.eof());
      }
    });
}

SEASTAR_TEST_CASE(test_more_than_requests_content) {
    return seastar::async([] {
      {
        auto inp = util::as_input_stream(temporary_buffer<char>::copy_of("asdfghjkl;1234567890xyz"));
        auto content_strm = input_stream<char>(data_source(std::make_unique<httpd::internal::content_length_source_impl>(inp, 20)));

        auto content1 = content_strm.read().get();
        BOOST_REQUIRE(temporary_buffer<char>("asdfghjkl;1234567890", 20) == content1);
        auto content2 = content_strm.read().get();
        BOOST_REQUIRE(temporary_buffer<char>() == content2);
        BOOST_REQUIRE(content_strm.eof());
        auto content3 = inp.read().get();
        BOOST_REQUIRE(temporary_buffer<char>("xyz", 3) == content3);
      }

      {
        auto inp = util::as_input_stream(temporary_buffer<char>::copy_of("4\r\n1324\r\n0\r\n\r\nxyz"));
        std::unordered_map<sstring, sstring> tmp, tmp2;
        auto content_strm = input_stream<char>(data_source(std::make_unique<httpd::internal::chunked_source_impl>(inp, tmp, tmp2)));

        auto content1 = content_strm.read().get();
        BOOST_REQUIRE(temporary_buffer<char>("1324", 4) == content1);
        auto content2 = content_strm.read().get();
        BOOST_REQUIRE(temporary_buffer<char>() == content2);
        BOOST_REQUIRE(content_strm.eof());
        auto content3 = inp.read().get();
        BOOST_REQUIRE(temporary_buffer<char>("xyz", 3) == content3);
      }
    });
}

class single_bytes_source_impl : public data_source_impl {
    temporary_buffer<char> _tmp;
public:
    single_bytes_source_impl(temporary_buffer<char> tmp)
        : _tmp(std::move(tmp)) {
    }
    virtual future<temporary_buffer<char>> get() override {
        if (_tmp.empty()) {
            return make_ready_future<temporary_buffer<char>>();
        }
        auto byte = _tmp.share(0, 1);
        _tmp.trim_front(1);
        return make_ready_future<temporary_buffer<char>>(std::move(byte));
    }
    virtual future<temporary_buffer<char>> skip(uint64_t n) override {
        _tmp.trim_front(std::min(_tmp.size(), n));
        return make_ready_future<temporary_buffer<char>>();
    }
};

SEASTAR_TEST_CASE(test_single_bytes_source) {
    return seastar::async([] {
        sstring input_str = "test input";
        auto ds = data_source(std::make_unique<single_bytes_source_impl>(temporary_buffer<char>(input_str.c_str(), input_str.size())));
        for (auto& ch : input_str) {
            temporary_buffer<char> one_letter_buf(1);
            *one_letter_buf.get_write() = ch;
            auto get_buf = ds.get().get();
            BOOST_REQUIRE(one_letter_buf == get_buf);
        }
    });
}

SEASTAR_TEST_CASE(test_fragmented_chunks) {
    // Test if a message that cannot be parsed as a http request is being replied with a 400 Bad Request response
    return seastar::async([] {
        sstring request_string = "a;chunk=ext\r\n1234567890\r\n0\r\ntrailer: part\r\n\r\n";
        auto inp = input_stream<char>(data_source(std::make_unique<single_bytes_source_impl>(temporary_buffer<char>(request_string.c_str(), request_string.size()))));
        std::unordered_map<sstring, sstring> chunk_extensions;
        std::unordered_map<sstring, sstring> trailing_headers;
        auto content_stream = input_stream<char>(data_source(std::make_unique<httpd::internal::chunked_source_impl>(inp, chunk_extensions, trailing_headers)));
        for (auto& ch : sstring("1234567890")) {
            temporary_buffer<char> one_letter_buf(1);
            *one_letter_buf.get_write() = ch;
            auto read_buf = content_stream.read().get();
            BOOST_REQUIRE(one_letter_buf == read_buf);
        }
        auto read_buf = content_stream.read().get();
        BOOST_REQUIRE(temporary_buffer<char>() == read_buf);
        BOOST_REQUIRE(chunk_extensions[sstring("chunk")] == sstring("ext"));
        BOOST_REQUIRE(trailing_headers[sstring("trailer")] == sstring("part"));
    });
}

SEASTAR_TEST_CASE(test_full_chunk_format) {
    return seastar::async([] {
        // Two data chunks with complex extensions and trailing headers
        auto inp = input_stream<char>(data_source(std::make_unique<buf_source_impl>(sstring(
            "a;abc-def;hello=world;aaaa\r\n1234567890\r\n"
            "a;a0-!#$%&'*+.^_`|~=\"quoted string obstext\x80\x81\xff quoted_pair: \\a\"\r\n1234521345\r\n"
            "0\r\na:b\r\n~|`_^.+*'&%$#!-0a:  ~!@#$%^&*()_+\x80\x81\xff\r\n  obs fold  \r\n\r\n"))));
        std::unordered_map<sstring, sstring> chunk_extensions;
        std::unordered_map<sstring, sstring> trailing_headers;
        auto content_stream = input_stream<char>(data_source(std::make_unique<httpd::internal::chunked_source_impl>(inp, chunk_extensions, trailing_headers)));

        sstring decoded_body;
        content_stream.consume([&decoded_body] (temporary_buffer<char> buf) {
            decoded_body += sstring(buf.get(), buf.size());
            return make_ready_future<consumption_result<char>>(continue_consuming{});
        }).get();

        BOOST_REQUIRE_EQUAL(decoded_body, sstring("12345678901234521345"));
        BOOST_REQUIRE_EQUAL(chunk_extensions[sstring("abc-def")], sstring(""));
        BOOST_REQUIRE_EQUAL(chunk_extensions[sstring("hello")], sstring("world"));
        BOOST_REQUIRE_EQUAL(chunk_extensions[sstring("aaaa")], sstring(""));
        BOOST_REQUIRE_EQUAL(chunk_extensions[sstring("a0-!#$%&'*+.^_`|~")], sstring("quoted string obstext\x80\x81\xff quoted_pair: a"));
        BOOST_REQUIRE_EQUAL(trailing_headers[sstring("a")], sstring("b"));
        BOOST_REQUIRE_EQUAL(trailing_headers[sstring("~|`_^.+*'&%$#!-0a")], sstring("~!@#$%^&*()_+\x80\x81\xff obs fold"));
    });
}

SEASTAR_TEST_CASE(test_chunk_extension_parser_fail) {
    return seastar::async([] {
        // Space after semicolon with no extension name is invalid
        auto inp = input_stream<char>(data_source(std::make_unique<buf_source_impl>(sstring("7; \r\nnoparse\r\n0\r\n\r\n"))));
        std::unordered_map<sstring, sstring> chunk_extensions;
        std::unordered_map<sstring, sstring> trailing_headers;
        auto content_stream = input_stream<char>(data_source(std::make_unique<httpd::internal::chunked_source_impl>(inp, chunk_extensions, trailing_headers)));
        BOOST_REQUIRE_EXCEPTION(content_stream.read().get(), httpd::bad_request_exception,
            [] (const httpd::bad_request_exception& e) {
                return sstring(e.what()).find("Can't parse chunk size and extensions") != sstring::npos;
            });
    });
}

SEASTAR_TEST_CASE(test_trailer_part_parser_fail) {
    return seastar::async([] {
        // Valid data chunk followed by a trailer with invalid syntax (= instead of :)
        auto inp = input_stream<char>(data_source(std::make_unique<buf_source_impl>(sstring(
            "8\r\nparsable\r\n0\r\ngood:header\r\nbad=header\r\n\r\n"))));
        std::unordered_map<sstring, sstring> chunk_extensions;
        std::unordered_map<sstring, sstring> trailing_headers;
        auto content_stream = input_stream<char>(data_source(std::make_unique<httpd::internal::chunked_source_impl>(inp, chunk_extensions, trailing_headers)));
        // Read the data chunk successfully first
        auto buf = content_stream.read().get();
        BOOST_REQUIRE_EQUAL(sstring(buf.get(), buf.size()), sstring("parsable"));
        // Next read triggers trailer parsing and must throw
        BOOST_REQUIRE_EXCEPTION(content_stream.read().get(), httpd::bad_request_exception,
            [] (const httpd::bad_request_exception& e) {
                return sstring(e.what()).find("Can't parse chunked request trailer") != sstring::npos;
            });
    });
}

SEASTAR_TEST_CASE(test_too_long_chunk) {
    return seastar::async([] {
        // Second chunk declares 10 bytes but has an extra 'X' after them
        auto inp = input_stream<char>(data_source(std::make_unique<buf_source_impl>(sstring(
            "a\r\n1234567890\r\na\r\n1234521345X\r\n0\r\n\r\n"))));
        std::unordered_map<sstring, sstring> chunk_extensions;
        std::unordered_map<sstring, sstring> trailing_headers;
        auto content_stream = input_stream<char>(data_source(std::make_unique<httpd::internal::chunked_source_impl>(inp, chunk_extensions, trailing_headers)));
        // Read both chunk bodies in one call
        auto buf = content_stream.read_exactly(20).get();
        BOOST_REQUIRE_EQUAL(sstring(buf.get(), buf.size()), sstring("12345678901234521345"));
        // Next read finds 'X' where '\r' is expected and must throw
        BOOST_REQUIRE_EXCEPTION(content_stream.read().get(), httpd::bad_chunk_exception,
            [] (const httpd::bad_chunk_exception& e) {
                return sstring(e.what()).find("The actual chunk length exceeds the specified length") != sstring::npos;
            });
    });
}

SEASTAR_TEST_CASE(test_bad_chunk_length) {
    return seastar::async([] {
        // Second chunk size contains an invalid hex character 'X'
        auto inp = input_stream<char>(data_source(std::make_unique<buf_source_impl>(sstring(
            "a\r\n1234567890\r\naX\r\n1234521345\r\n0\r\n\r\n"))));
        std::unordered_map<sstring, sstring> chunk_extensions;
        std::unordered_map<sstring, sstring> trailing_headers;
        auto content_stream = input_stream<char>(data_source(std::make_unique<httpd::internal::chunked_source_impl>(inp, chunk_extensions, trailing_headers)));
        // Read first chunk successfully
        auto buf = content_stream.read_exactly(10).get();
        BOOST_REQUIRE_EQUAL(sstring(buf.get(), buf.size()), sstring("1234567890"));
        // Next read tries to parse 'aX' as chunk size and must throw
        BOOST_REQUIRE_EXCEPTION(content_stream.read().get(), httpd::bad_request_exception,
            [] (const httpd::bad_request_exception& e) {
                return sstring(e.what()).find("Can't parse chunk size and extensions") != sstring::npos;
            });
    });
}
