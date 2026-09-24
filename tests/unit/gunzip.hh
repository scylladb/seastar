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
 * Copyright (C) 2026 ScyllaDB
 */

#pragma once

#include <zlib.h>

#include <stdexcept>
#include <string>
#include <vector>

// Decompresses a gzip stream (or with raw, the deflate stream within it,
// ignoring the header and trailer).
inline std::string gunzip(const char* data, size_t size, bool raw = false) {
    z_stream zs{};
    if (raw) {
        data += 10;
        size -= 18;
    }
    if (inflateInit2(&zs, raw ? -MAX_WBITS : 16 + MAX_WBITS) != Z_OK) {
        throw std::runtime_error("inflateInit2");
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data));
    zs.avail_in = size;
    std::string out;
    std::vector<char> buf(65536);
    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf.data());
        zs.avail_out = buf.size();
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            throw std::runtime_error(std::string("inflate: ") + (zs.msg ? zs.msg : "?"));
        }
        out.append(buf.data(), buf.size() - zs.avail_out);
    } while (ret != Z_STREAM_END);
    if (zs.avail_in) {
        inflateEnd(&zs);
        throw std::runtime_error("inflate: trailing data");
    }
    inflateEnd(&zs);
    return out;
}
