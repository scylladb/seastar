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
 * Copyright (C) 2016 Scylladb, Ltd.
 */

#include "lz4_compressor.hh"
#include "core/byteorder.hh"

namespace seastar {

namespace rpc {

const sstring lz4_compressor::factory::_name = "LZ4";


static temporary_buffer<char> linearize(boost::variant<std::vector<temporary_buffer<char>>, temporary_buffer<char>>& v, uint32_t size) {
    auto* one = boost::get<temporary_buffer<char>>(&v);
    if (one) {
        // no need to linearize
        return std::move(*one);
    } else {
        temporary_buffer<char> src(size);
        auto p = src.get_write();
        for (auto&& b : boost::get<std::vector<temporary_buffer<char>>>(v)) {
            p = std::copy_n(b.begin(), b.size(), p);
        }
        return src;
    }
}

snd_buf lz4_compressor::compress(size_t head_space, snd_buf data) {
    head_space += 4;
    temporary_buffer<char> dst(head_space + LZ4_compressBound(data.size));
    temporary_buffer<char> src = linearize(data.bufs, data.size);
#ifdef HAVE_LZ4_COMPRESS_DEFAULT
    auto size = LZ4_compress_default(src.begin(), dst.get_write() + head_space, src.size(), LZ4_compressBound(src.size()));
#else
    // Safe since output buffer is sized properly.
    auto size = LZ4_compress(src.begin(), dst.get_write() + head_space, src.size());
#endif
    if (size == 0) {
        throw std::runtime_error("RPC frame LZ4 compression failure");
    }
    dst.trim(size + head_space);
    write_le<uint32_t>(dst.get_write() + (head_space - 4), data.size);
    return snd_buf(std::move(dst));
}

rcv_buf lz4_compressor::decompress(rcv_buf data) {
    if (data.size < 4) {
        return rcv_buf();
    } else {
        auto in = make_deserializer_stream(data);
        uint32_t v32;
        in.read(reinterpret_cast<char*>(&v32), 4);
        auto size = le_to_cpu(v32);
        if (size) {
            temporary_buffer<char> src = linearize(data.bufs, data.size);
            src.trim_front(4);
            rcv_buf rb(size);
            rb.bufs = temporary_buffer<char>(size);
            auto& dst = boost::get<temporary_buffer<char>>(rb.bufs);
            if (LZ4_decompress_fast(src.begin(), dst.get_write(), dst.size()) < 0) {
                throw std::runtime_error("RPC frame LZ4 decompression failure");
            }
            return rb;
        } else {
            // special case: if uncompressed size is zero it means that data was not compressed
            // compress side still not use this but we want to be ready for the future
            return data;
        }
    }
}

}

}
