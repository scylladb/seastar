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

#include <seastar/http/common.hh>
#include <seastar/core/iostream-impl.hh>

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
    return "GET";
}

}

namespace http {
namespace internal {

class http_chunked_data_sink_impl : public data_sink_impl {
    output_stream<char>& _out;

    future<> write_size(size_t s) {
        auto req = format("{:x}\r\n", s);
        return _out.write(req);
    }
public:
    http_chunked_data_sink_impl(output_stream<char>& out) : _out(out) {
    }
    virtual future<> put(net::packet data)  override { abort(); }
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
    return output_stream<char>(http_chunked_data_sink(out), 32000, opts);
}

}
}

}

