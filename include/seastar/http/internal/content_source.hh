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

#include <seastar/http/chunk_parsers.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/http/common.hh>
#include <seastar/util/log.hh>
#include <seastar/http/exception.hh>

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
        return _inp.skip(skip_bytes).then([] {
            return temporary_buffer<char>();
        });
    }

    virtual future<> close() override {
        return make_ready_future<>();
    }
};

/*
 * An input_stream wrapper that decodes a request body
 * with "chunked" encoding.
 * */
class chunked_source_impl : public data_source_impl {
    class chunk_parser {
        enum class parsing_state
            : uint8_t {
                size_and_ext,
                body,
                trailer_part
        };
        http_chunk_size_and_ext_parser _size_and_ext_parser;
        http_chunk_trailer_parser _trailer_parser;

        temporary_buffer<char> _buf;
        size_t _current_chunk_bytes_read = 0;
        size_t _current_chunk_length;
        parsing_state _ps = parsing_state::size_and_ext;
        bool _end_of_request = false;
        // references to fields in the request structure
        std::unordered_map<sstring, sstring>& _chunk_extensions;
        std::unordered_map<sstring, sstring>& _trailing_headers;
        using consumption_result_type = consumption_result<char>;
    public:
        chunk_parser(std::unordered_map<sstring, sstring>& chunk_extensions, std::unordered_map<sstring, sstring>& trailing_headers)
            : _chunk_extensions(chunk_extensions), _trailing_headers(trailing_headers) {
                _size_and_ext_parser.init();
        }
        temporary_buffer<char> buf() {
            _current_chunk_bytes_read += _buf.size();
            return std::move(_buf);
        }

        future<consumption_result_type> operator()(temporary_buffer<char> data) {
            if (_buf.size() || _end_of_request || data.empty()) {
                // return if we have already read some content (_buf.size()), we have already reached the end of the chunked request (_end_of_request),
                // or the underlying stream reached eof (data.empty())
                return make_ready_future<consumption_result_type>(stop_consuming(std::move(data)));
            }
            switch (_ps) {
            // "data" buffer is non-empty
            case parsing_state::size_and_ext:
                return _size_and_ext_parser(std::move(data)).then([this] (std::optional<temporary_buffer<char>> res) {
                    if (res.has_value()) {
                        if (_size_and_ext_parser.failed()) {
                            return make_exception_future<consumption_result_type>(bad_request_exception("Can't parse chunk size and extensions"));
                        }
                        // save extensions
                        auto parsed_extensions = _size_and_ext_parser.get_parsed_extensions();
                        _chunk_extensions.merge(parsed_extensions);
                        for (auto& key_val : parsed_extensions) {
                            _chunk_extensions[key_val.first] += sstring(",") + key_val.second;
                        }

                        // save size
                        auto size_string = _size_and_ext_parser.get_size();
                        if (size_string.size() > 16) {
                            return make_exception_future<consumption_result_type>(bad_chunk_exception("Chunk length too big"));
                        }
                        _current_chunk_bytes_read = 0;
                        _current_chunk_length = strtol(size_string.c_str(), nullptr, 16);

                        if (_current_chunk_length == 0) {
                            _ps = parsing_state::trailer_part;
                            _trailer_parser.init();
                        } else {
                            _ps = parsing_state::body;
                        }
                        if (res->empty()) {
                            return make_ready_future<consumption_result_type>(continue_consuming{});
                        }
                        return this->operator()(std::move(res.value()));
                    } else {
                        return make_ready_future<consumption_result_type>(continue_consuming{});
                    }
                });
            case parsing_state::body:
                // read the new data into _buf
                if (_current_chunk_bytes_read < _current_chunk_length) {
                    size_t to_read = std::min(_current_chunk_length - _current_chunk_bytes_read, data.size());
                    if (_buf.empty()) {
                        _buf = data.share(0, to_read);
                    }
                    data.trim_front(to_read);
                    return make_ready_future<consumption_result_type>(stop_consuming(std::move(data)));
                }

                // chunk body is finished, we haven't entered the previous if, so "data" is still non-empty
                if (_current_chunk_bytes_read == _current_chunk_length) {
                    // we haven't read \r yet
                    if (data.get()[0] != '\r') {
                        return make_exception_future<consumption_result_type>(bad_chunk_exception("The actual chunk length exceeds the specified length"));
                    } else {
                        _current_chunk_bytes_read++;
                        data.trim_front(1);
                        if (data.empty()) {
                            return make_ready_future<consumption_result_type>(continue_consuming{});
                        }
                    }
                }
                if (_current_chunk_bytes_read == _current_chunk_length + 1) {
                    // we haven't read \n but have \r
                    if (data.get()[0] != '\n') {
                        return make_exception_future<consumption_result_type>(bad_chunk_exception("The actual chunk length exceeds the specified length"));
                    } else {
                        _ps = parsing_state::size_and_ext;
                        _size_and_ext_parser.init();
                        data.trim_front(1);
                        if (data.empty()) {
                            return make_ready_future<consumption_result_type>(continue_consuming{});
                        }
                    }
                }
                return this->operator()(std::move(data));
            case parsing_state::trailer_part:
                return _trailer_parser(std::move(data)).then([this] (std::optional<temporary_buffer<char>> res) {
                    if (res.has_value()) {
                        if (_trailer_parser.failed()) {
                            return make_exception_future<consumption_result_type>(bad_request_exception("Can't parse chunked request trailer"));
                        }
                        // save trailing headers
                        _trailing_headers = _trailer_parser.get_parsed_headers();
                        _end_of_request = true;
                        return make_ready_future<consumption_result_type>(stop_consuming(std::move(*res)));
                    } else {
                        return make_ready_future<consumption_result_type>(continue_consuming{});
                    }
                });
            }
            __builtin_unreachable();
        }
    };
    input_stream<char>& _inp;
    chunk_parser _chunk;

public:
    chunked_source_impl(input_stream<char>& inp, std::unordered_map<sstring, sstring>& chunk_extensions, std::unordered_map<sstring, sstring>& trailing_headers)
        : _inp(inp), _chunk(chunk_extensions, trailing_headers) {
    }

    virtual future<temporary_buffer<char>> get() override {
        return _inp.consume(_chunk).then([this] () mutable {
            return _chunk.buf();
        });
    }

    virtual future<> close() override {
        return make_ready_future<>();
    }
};

} // namespace internal

} // namespace httpd

}
