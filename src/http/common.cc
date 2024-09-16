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

#ifdef SEASTAR_MODULE
module;
#endif

#include <cstdlib>
#include <memory>
#include <utility>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/http/common.hh>
#include <seastar/core/iostream-impl.hh>
#endif

namespace seastar {

namespace httpd {

operation_type str2type(const sstring& type) {
    if (type == "DELETE") {
        return DELETE;
    }
    if (type == "POST") {
        return POST;
    }
    if (type == "PUT") {
        return PUT;
    }
    if (type == "HEAD") {
        return HEAD;
    }
    if (type == "OPTIONS") {
        return OPTIONS;
    }
    if (type == "TRACE") {
        return TRACE;
    }
    if (type == "CONNECT") {
        return CONNECT;
    }
    if (type == "PATCH") {
        return PATCH;
    }
    return GET;
}

sstring type2str(operation_type type) {
    if (type == DELETE) {
        return "DELETE";
    }
    if (type == POST) {
        return "POST";
    }
    if (type == PUT) {
        return "PUT";
    }
    if (type == HEAD) {
        return "HEAD";
    }
    if (type == OPTIONS) {
        return "OPTIONS";
    }
    if (type == TRACE) {
        return "TRACE";
    }
    if (type == CONNECT) {
        return "CONNECT";
    }
    if (type == PATCH) {
        return "PATCH";
    }
    return "GET";
}

}

namespace http {
namespace internal {

static constexpr size_t default_body_sink_buffer_size = 32000;

// Data sinks below are running "on top" of socket output stream and provide
// reliable and handy way of generating request bodies according to selected
// encoding type and content-length.
//
// Respectively, both .close() methods should not close the underlying stream,
// because the socket in question may continue being in use for keep-alive
// connections, and closing it would just break the keep-alive-ness

class http_chunked_data_sink_impl : public data_sink_impl {
    output_stream<char>& _out;

    future<> write_size(size_t s) {
        auto req = format("{:x}\r\n", s);
        return _out.write(req);
    }
public:
    http_chunked_data_sink_impl(output_stream<char>& out) : _out(out) {
    }
    virtual future<> put(net::packet data) override {
        return data_sink_impl::fallback_put(std::move(data));
    }
    using data_sink_impl::put;
    virtual future<> put(temporary_buffer<char> buf) override {
        if (buf.size() == 0) {
            // size 0 buffer should be ignored, some server
            // may consider it an end of message
            return make_ready_future<>();
        }
        auto size = buf.size();
        return write_size(size).then([this, buf = std::move(buf)] () mutable {
            return _out.write(buf.get(), buf.size());
        }).then([this] () mutable {
            return _out.write("\r\n", 2);
        });
    }
    virtual future<> close() override {
        return  make_ready_future<>();
    }
};

class http_chunked_data_sink : public data_sink {
public:
    http_chunked_data_sink(output_stream<char>& out)
        : data_sink(std::make_unique<http_chunked_data_sink_impl>(
                out)) {}
};

output_stream<char> make_http_chunked_output_stream(output_stream<char>& out) {
    output_stream_options opts;
    opts.trim_to_size = true;
    return output_stream<char>(http_chunked_data_sink(out), default_body_sink_buffer_size, opts);
}

class http_content_length_data_sink_impl : public data_sink_impl {
    output_stream<char>& _out;
    const size_t _limit;
    size_t& _bytes_written;

public:
    http_content_length_data_sink_impl(output_stream<char>& out, size_t total_len, size_t& bytes_written)
            : _out(out)
            , _limit(total_len)
            , _bytes_written(bytes_written)
    {
        // at the very beginning, 0 bytes were written
        _bytes_written = 0;
    }
    virtual future<> put(net::packet data) override {
        return data_sink_impl::fallback_put(std::move(data));
    }
    using data_sink_impl::put;
    virtual future<> put(temporary_buffer<char> buf) override {
        if (buf.size() == 0 || _bytes_written == _limit) {
            return make_ready_future<>();
        }

        auto size = buf.size();
        if (_bytes_written + size > _limit) {
            return make_exception_future<>(std::runtime_error(format("body conent length overflow: want {} limit {}", _bytes_written + buf.size(), _limit)));
        }

        return _out.write(buf.get(), size).then([this, size] {
            _bytes_written += size;
        });
    }
    virtual future<> close() override {
        return make_ready_future<>();
    }
};

class http_content_length_data_sink : public data_sink {
public:
    http_content_length_data_sink(output_stream<char>& out, size_t total_len, size_t& bytes_written)
        : data_sink(std::make_unique<http_content_length_data_sink_impl>(out, total_len, bytes_written))
    {
    }
};

output_stream<char> make_http_content_length_output_stream(output_stream<char>& out, size_t total_len, size_t& bytes_written) {
    output_stream_options opts;
    opts.trim_to_size = true;
    return output_stream<char>(http_content_length_data_sink(out, total_len, bytes_written), default_body_sink_buffer_size, opts);
}

}
}

}

