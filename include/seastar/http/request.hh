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
 * Copyright 2015 Cloudius Systems
 */

//
// request.hpp
// ~~~~~~~~~~~
//
// Copyright (c) 2003-2013 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include <seastar/core/sstring.hh>
#include <string>
#include <vector>
#include <strings.h>
#include <seastar/http/common.hh>
#include <seastar/core/iostream.hh>

namespace seastar {

namespace httpd {
class connection;

/**
 * A request received from a client.
 */
struct request {
    enum class ctclass
        : char {
            other, multipart, app_x_www_urlencoded,
    };

    struct case_insensitive_cmp {
        bool operator()(const sstring& s1, const sstring& s2) const {
            return std::equal(s1.begin(), s1.end(), s2.begin(), s2.end(),
                    [](char a, char b) { return ::tolower(a) == ::tolower(b); });
        }
    };

    struct case_insensitive_hash {
        size_t operator()(sstring s) const {
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            return std::hash<sstring>()(s);
        }
    };

    sstring _method;
    sstring _url;
    sstring _version;
    int http_version_major;
    int http_version_minor;
    ctclass content_type_class;
    size_t content_length = 0;
    std::unordered_map<sstring, sstring, case_insensitive_hash, case_insensitive_cmp> _headers;
    std::unordered_map<sstring, sstring> query_parameters;
    connection* connection_ptr;
    parameters param;
    sstring content;
    sstring protocol_name = "http";

    /**
     * Search for the first header of a given name
     * @param name the header name
     * @return a pointer to the header value, if it exists or empty string
     */
    sstring get_header(const sstring& name) const {
        auto res = _headers.find(name);
        if (res == _headers.end()) {
            return "";
        }
        return res->second;
    }

    /**
     * Search for the first header of a given name
     * @param name the header name
     * @return a pointer to the header value, if it exists or empty string
     */
    sstring get_query_param(const sstring& name) const {
        auto res = query_parameters.find(name);
        if (res == query_parameters.end()) {
            return "";
        }
        return res->second;
    }

    /**
     * Get the request protocol name. Can be either "http" or "https".
     */
    sstring get_protocol_name() const {
        return protocol_name;
    }

    /**
     * Get the request url.
     * @return the request url
     */
    sstring get_url() const {
        return get_protocol_name() + "://" + get_header("Host") + _url;
    }

    bool is_multi_part() const {
        return content_type_class == ctclass::multipart;
    }

    bool is_form_post() const {
        return content_type_class == ctclass::app_x_www_urlencoded;
    }

};

/**
 * Helper class for reading entire streams, for example streams containing content of a HTTP request
 */
class short_stream_reader {
public:
    /// Returns all bytes from the stream until eof, accessible in chunks
    static future<std::vector<temporary_buffer<char>>> read_entire_stream(input_stream<char>& inp) {
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

    /// Returns all bytes from the stream until eof as a single buffer, use only on short streams
    static future<sstring> read_entire_stream_contiguous(input_stream<char>& inp) {
        return read_entire_stream(inp).then([&inp] (std::vector<temporary_buffer<char>> bufs) {
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

    /// Ignores all bytes until eof
    static future<> skip_entire_stream(input_stream<char>& inp) {
        return inp.consume([] (temporary_buffer<char> tmp) {
            return tmp.empty() ? make_ready_future<consumption_result<char>>(stop_consuming(temporary_buffer<char>()))
                            : make_ready_future<consumption_result<char>>(continue_consuming());
        });
    }
};


} // namespace httpd

}
