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
 * Copyright (C) 2023 Scylladb, Ltd.
 */

#include <seastar/core/byteorder.hh>
#include <seastar/rpc/zstd_streaming_compressor.hh>

namespace seastar {
namespace rpc {

sstring zstd_streaming_compressor::name() const {
    return factory{}.supported();
}

const sstring& zstd_streaming_compressor::factory::supported() const {
    const static sstring name = "ZSTD_STREAMING";
    return name;
}

std::unique_ptr<rpc::compressor> zstd_streaming_compressor::factory::negotiate(sstring feature, bool is_server) const {
    return feature == supported() ? std::make_unique<zstd_streaming_compressor>() : nullptr;
}

constexpr size_t chunk_size = snd_buf::chunk_size;

snd_buf zstd_streaming_compressor::compress(size_t head_space, snd_buf data) {
    const auto size = data.size;
    auto src = std::get_if<temporary_buffer<char>>(&data.bufs);
    if (!src) {
        src = std::get<std::vector<temporary_buffer<char>>>(data.bufs).data();
    }

    std::vector<temporary_buffer<char>> dst_buffers;
    constexpr size_t header_size = 4;
    dst_buffers.emplace_back(std::max<size_t>(head_space + header_size, chunk_size));
    char* const header = dst_buffers.back().get_write() + head_space;

    size_t size_left = size;
    size_left -= src->size();
    size_t size_compressed = 0;

    ZSTD_inBuffer inbuf;
    ZSTD_outBuffer outbuf;

    inbuf.src = src->get();
    inbuf.size = src->size();
    inbuf.pos = 0;

    outbuf.dst = dst_buffers.back().get_write();
    outbuf.size = dst_buffers.back().size();
    outbuf.pos = head_space + header_size;

    if (size > 0) {
        while (true) {
            if (size_left && inbuf.pos == inbuf.size) {
                ++src;
                size_left -= src->size();
                inbuf.src = src->get();
                inbuf.size = src->size();
                inbuf.pos = 0;
                continue;
            }
            if (outbuf.pos == outbuf.size) {
                size_compressed += outbuf.pos;
                dst_buffers.emplace_back(chunk_size);
                outbuf.dst = dst_buffers.back().get_write();
                outbuf.size = dst_buffers.back().size();
                outbuf.pos = 0;
                continue;
            }
            size_t ret = ZSTD_compressStream2(_cstream.get(), &outbuf, &inbuf, static_cast<ZSTD_EndDirective>(!size_left));
            check_zstd(ret, "ZSTD_compressStream2");
            if (!size_left && inbuf.pos == inbuf.size && ret == 0) {
                break;
            }
        }
    }
    size_compressed += outbuf.pos;
    dst_buffers.back().trim(outbuf.pos);

    write_le<uint32_t>(header, size);

    // If messages are small, the chunk_size per message might be too big.
    // Let's pay some cycles to shrink the underlying allocation to fit,
    // to avoid problems with that overhead.
    dst_buffers.back() = dst_buffers.back().clone();
    if (dst_buffers.size() == 1) {
        return snd_buf(std::move(dst_buffers.front()));
    }
    return snd_buf(std::move(dst_buffers), size_compressed);
}

rcv_buf zstd_streaming_compressor::decompress(rcv_buf data) {
    const auto size = data.size;
    if (size < 4) {
        return rcv_buf();
    }

    auto src = std::get_if<temporary_buffer<char>>(&data.bufs);
    if (!src) {
        src = std::get<std::vector<temporary_buffer<char>>>(data.bufs).data();
    }

    size_t size_left = size;
    size_left -= src->size();
    size_t size_decompressed = 0;

    constexpr size_t header_size = 4;
    char header[header_size];

    ZSTD_inBuffer inbuf;
    ZSTD_outBuffer outbuf;

    inbuf.src = src->get();
    inbuf.size = src->size();
    inbuf.pos = 0;

    outbuf.dst = std::data(header);
    outbuf.size = std::size(header);
    outbuf.pos = 0;

    while (outbuf.pos != outbuf.size) {
        if (size_left && inbuf.pos == inbuf.size) {
            ++src;
            size_left -= src->size();
            inbuf.src = src->get();
            inbuf.size = src->size();
            inbuf.pos = 0;
            continue;
        }
        size_t n = std::min(outbuf.size - outbuf.pos, inbuf.size - inbuf.pos);
        memcpy(static_cast<char*>(outbuf.dst) + outbuf.pos, static_cast<const char*>(inbuf.src) + inbuf.pos, n);
        outbuf.pos += n;
        inbuf.pos += n;
    }

    const size_t expected_size_decompressed = read_le<uint32_t>(header);

    std::vector<temporary_buffer<char>> dst_buffers;
    dst_buffers.emplace_back(std::min<size_t>(expected_size_decompressed - size_decompressed, chunk_size));
    outbuf.dst = dst_buffers.back().get_write();
    outbuf.size = dst_buffers.back().size();
    outbuf.pos = 0;

    if (size - header_size > 0) {
        while (true) {
            if (size_left && inbuf.pos == inbuf.size) {
                ++src;
                size_left -= src->size();
                inbuf.src = src->get();
                inbuf.size = src->size();
                inbuf.pos = 0;
                continue;
            }
            if (outbuf.pos == outbuf.size) {
                size_decompressed += outbuf.pos;
                // The blessed check that the decompressor finished its work is `outbuf.pos < outbuf.size`.
                // To perform that check, we have to give the decompressor at least one extra byte of output space.
                // Hence `+ 1`.
                dst_buffers.emplace_back(std::min<size_t>(expected_size_decompressed - size_decompressed + 1, chunk_size));
                outbuf.dst = dst_buffers.back().get_write();
                outbuf.size = dst_buffers.back().size();
                outbuf.pos = 0;
                continue;
            }
            size_t ret = ZSTD_decompressStream(_dstream.get(), &outbuf, &inbuf);
            check_zstd(ret, "ZSTD_decompressStream");
            if (!size_left // No more input chunks.
                && inbuf.pos == inbuf.size // No more data in the last input chunk.
                && (outbuf.pos < outbuf.size // No more data in decompressor's internal buffers.
                    || outbuf.pos + size_decompressed > expected_size_decompressed) // The decompressed message is bigger than promised.
            ) {
                break;
            }
        }
    }
    size_decompressed += outbuf.pos;
    dst_buffers.back().trim(outbuf.pos);
    if (size_decompressed != expected_size_decompressed) {
        throw std::runtime_error(fmt::format("ZSTD decompressed frame size {} doesn't match pledged size {}", size_decompressed, expected_size_decompressed));
    }

    if (dst_buffers.size() == 1) {
        return rcv_buf(std::move(dst_buffers.front()));
    }
    return rcv_buf(std::move(dst_buffers), size_decompressed);
}

}
}
