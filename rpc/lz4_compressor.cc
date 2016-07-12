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

namespace rpc {

const sstring lz4_compressor::factory::_name = "LZ4";

sstring lz4_compressor::compress(size_t head_space, sstring data) {
    head_space += 4;
    sstring dst(sstring::initialized_later(), head_space + LZ4_compressBound(data.size()));
    // Can't use LZ4_compress_default() since it's too new.
    // Safe since output buffer is sized properly.
    auto size = LZ4_compress(data.begin(), dst.begin() + head_space, data.size());
    dst.resize(size + head_space);
    *unaligned_cast<uint32_t*>(dst.data() + 4) = cpu_to_le(data.size());
    return dst;
}

temporary_buffer<char> lz4_compressor::decompress(temporary_buffer<char> data) {
    if (data.size() < 4) {
        return temporary_buffer<char>();
    } else {
        auto size = le_to_cpu(*unaligned_cast<uint32_t*>(data.begin()));
        if (size) {
            temporary_buffer<char> dst(size);
            LZ4_decompress_fast(data.begin() + 4, dst.get_write(), dst.size());
            return dst;
        } else {
            // special case: if uncompressed size is zero it means that data was not compressed
            // compress side still not use this but we want to be ready for the future
            data.trim_front(4);
            return std::move(data);
        }
    }
}

}
