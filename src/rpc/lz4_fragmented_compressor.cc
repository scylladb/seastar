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
 * Copyright (C) 2019 Scylladb, Ltd.
 */

#include <seastar/rpc/lz4_fragmented_compressor.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/util/assert.hh>

#include <lz4.h>
// LZ4_DECODER_RING_BUFFER_SIZE macro is introduced since v1.8.2
// To work with previous lz4 release, copied the definition in lz4 here
#ifndef LZ4_DECODER_RING_BUFFER_SIZE
#define LZ4_DECODER_RING_BUFFER_SIZE(maxBlockSize) (65536 + 14 + (maxBlockSize))
#endif

namespace seastar {
namespace rpc {

sstring lz4_fragmented_compressor::name() const {
    return factory{}.supported();
}

const sstring& lz4_fragmented_compressor::factory::supported() const {
    const static sstring name = "LZ4_FRAGMENTED";
    return name;
}

std::unique_ptr<rpc::compressor> lz4_fragmented_compressor::factory::negotiate(sstring feature, bool is_server) const {
    return feature == supported() ? std::make_unique<lz4_fragmented_compressor>() : nullptr;
}

// Compressed message format:
// The message consists of one or more data chunks each preceeded by a 4 byte header.
// The value of the header detrmines the size of the chunk:
// - most significant bit is cleared: intermediate chunk, 31 least significant bits
//   contain the compressed size of the chunk (i.e. how it appears on wire), the
//   decompressed size is 32 kB.
// - most significant bit is set: last chunk, 31 least significant bits contain the
//   decompressed size of the chunk, the compressed size is the remaining part of
//   the message.
// Compression and decompression is done using LZ4 streaming interface. Each chunk
// depends on the one that precedes it.
// All metadata is little-endian.

static constexpr uint32_t last_chunk_flag = uint32_t(1) << 31;
static constexpr size_t chunk_header_size = sizeof(uint32_t);
static constexpr size_t chunk_size = 32 * 1024;

namespace {

struct compression_stream_deleter {
    void operator()(LZ4_stream_t* stream) const noexcept {
        LZ4_freeStream(stream);
    }
};

struct decompression_stream_deleter {
    void operator()(LZ4_streamDecode_t* stream) const noexcept {
        LZ4_freeStreamDecode(stream);
    }
};

}

snd_buf lz4_fragmented_compressor::compress(size_t head_space, snd_buf data) {
    static thread_local auto stream = std::unique_ptr<LZ4_stream_t, compression_stream_deleter>(LZ4_createStream());
    static_assert(chunk_size <= snd_buf::chunk_size, "chunk_size <= snd_buf::chunk_size");

    LZ4_resetStream(stream.get());

    auto size_left = data.size;
    auto src = std::get_if<temporary_buffer<char>>(&data.bufs);
    if (!src) {
        src = std::get<std::vector<temporary_buffer<char>>>(data.bufs).data();
    }

    auto single_chunk_size = LZ4_COMPRESSBOUND(size_left) + head_space + chunk_header_size;
    if (single_chunk_size <= chunk_size && size_left <= chunk_size && src->size() == size_left) {
        // faster path for small messages
        auto dst = temporary_buffer<char>(single_chunk_size);
        auto header = dst.get_write() + head_space;
        auto compressed_data = header + chunk_header_size;
        auto compressed_size = LZ4_compress_fast_continue(stream.get(), src->get(), compressed_data, size_left, LZ4_COMPRESSBOUND(size_left), 0);
        write_le(header, last_chunk_flag | size_left);
        dst.trim(head_space + chunk_header_size + compressed_size);
        return snd_buf(std::move(dst));
    }

    static constexpr size_t chunk_compress_bound = LZ4_COMPRESSBOUND(chunk_size);
    static constexpr size_t chunk_maximum_compressed_size = chunk_compress_bound + chunk_header_size;
    static_assert(chunk_maximum_compressed_size < snd_buf::chunk_size, "chunk_maximum_compressed_size is too large");

    std::vector<temporary_buffer<char>> dst_buffers;
    size_t dst_offset = head_space;

    dst_buffers.emplace_back(std::max<size_t>(head_space, snd_buf::chunk_size));

    // Intermediate chunks
    size_t total_compressed_size = 0;
    auto src_left = data.size;
    size_t src_current_offset = 0;

    // Advance offset in the current source fragment, move to the next fragment if needed.
    auto advance_src = [&] (size_t n) {
        src_current_offset += n;
        if (src_current_offset >= src->size()) {
            ++src;
            src_current_offset = 0;
        }
        src_left -= n;
    };

    // Input chunks do not have to be multiplies of chunk_size.
    // We handle such cases by reassembling a chunk in this temporary buffer.
    // Note that this is similar to the ring buffer compression case in docs,
    // we need to ensure that a suitable amount of (maybe) previous data is
    // stable in this or input buffer, thus make the temp buffer
    // LZ4_DECODER_RING_BUFFER_SIZE(chunk_size) large, and treat it as a ring.
    static constexpr auto lin_buf_size = LZ4_DECODER_RING_BUFFER_SIZE(chunk_size);
    static thread_local char temporary_chunk_data[lin_buf_size];
    size_t lin_off = 0;

    auto maybe_linearize = [&](size_t size) {
        auto src_ptr = src->get() + src_current_offset;
        if (src->size() - src_current_offset < size) {
            auto left = size;
            SEASTAR_ASSERT(lin_buf_size > size);
            if (lin_buf_size - lin_off < size) {
                lin_off = 0;
            }
            auto tmp = temporary_chunk_data + std::exchange(lin_off, lin_off + size);
            src_ptr = tmp;
            while (left) {
                auto this_size = std::min(src->size() - src_current_offset, left);
                tmp = std::copy_n(src->get() + src_current_offset, this_size, tmp);
                left -= this_size;
                advance_src(this_size);
            }
        } else {
            advance_src(chunk_size);
            lin_off = 0;
        }
        return src_ptr;
    };

    while (src_left > chunk_size) {
        // Check if we can fit another chunk in the current destination fragment.
        // If not allocate a new one.
        if (dst_offset + chunk_maximum_compressed_size > dst_buffers.back().size()) {
            dst_buffers.back().trim(dst_offset);
            dst_buffers.emplace_back(snd_buf::chunk_size);
            dst_offset = 0;
        }

        // Check if there is at least a contiguous chunk_size of data in the current
        // source fragment. If not, linearise that into temporary_chunk_data.
        auto src_ptr = maybe_linearize(chunk_size);
        auto header = dst_buffers.back().get_write() + dst_offset;
        auto dst = header + chunk_header_size;

        auto compressed_size = LZ4_compress_fast_continue(stream.get(), src_ptr, dst, chunk_size, chunk_compress_bound, 0);
        total_compressed_size += compressed_size + chunk_header_size;

        dst_offset += compressed_size + chunk_header_size;
        write_le<uint32_t>(header, compressed_size);
    }

    // Last chunk
    auto last_chunk_compress_bound = LZ4_COMPRESSBOUND(src_left);
    auto last_chunk_maximum_compressed_size = last_chunk_compress_bound + chunk_header_size;

    // Check if we can fit the last chunk in the current destination fragment. Allocate a new one if not.
    if (dst_offset + last_chunk_maximum_compressed_size > dst_buffers.back().size()) {
        dst_buffers.back().trim(dst_offset);
        dst_buffers.emplace_back(snd_buf::chunk_size);
        dst_offset = 0;
    }
    auto header = dst_buffers.back().get_write() + dst_offset;
    auto dst = header + chunk_header_size;

    // Check if all remaining source data is contiguous. If not linearise it into temporary_chunk_data.
    auto rem = src_left;
    auto src_ptr = maybe_linearize(src_left);

    auto compressed_size = LZ4_compress_fast_continue(stream.get(), src_ptr, dst, rem, last_chunk_compress_bound, 0);
    dst_offset += compressed_size + chunk_header_size;
    write_le<uint32_t>(header, last_chunk_flag | rem);
    total_compressed_size += compressed_size + chunk_header_size + head_space;

    auto& last = dst_buffers.back();
    last.trim(dst_offset);

    if (dst_buffers.size() == 1) {
        return snd_buf(std::move(dst_buffers.front()));
    }
    return snd_buf(std::move(dst_buffers), total_compressed_size);
}

rcv_buf lz4_fragmented_compressor::decompress(rcv_buf data) {
    if (data.size < 4) {
        return rcv_buf();
    }

    static thread_local auto stream = std::unique_ptr<LZ4_streamDecode_t, decompression_stream_deleter>(LZ4_createStreamDecode());

    if (!LZ4_setStreamDecode(stream.get(), nullptr, 0)) {
        throw std::runtime_error("RPC frame LZ4_FRAGMENTED decompression failed to reset state");
    }

    auto src = std::get_if<temporary_buffer<char>>(&data.bufs);
    size_t src_left = data.size;
    size_t src_offset = 0;

    // Prepare source data. Returns pointer to n contiguous bytes of source data.
    // Avoids copy if possible, otherwise uses dst as a temporary storage.
    auto copy_src = [&] (char* dst, size_t n) -> const char* {
        // Fast path, no need to copy anything.
        if (src->size() - src_offset >= n) {
            auto ptr = src->get() + src_offset;
            src_left -= n;
            src_offset += n;
            return ptr;
        }

        // Need to linearise source chunk into dst.
        auto ptr = dst;
        src_left -= n;
        while (n) {
            if (src_offset == src->size()) {
                ++src;
                src_offset = 0;
            }
            auto this_size = std::min(n, src->size() - src_offset);
            std::copy_n(src->get() + src_offset, this_size, dst);
            n -= this_size;
            dst += this_size;
            src_offset += this_size;
        }
        return ptr;
    };

    // Read, possibly fragmented, header.
    auto read_header = [&] {
        uint32_t header_value;
        auto ptr = copy_src(reinterpret_cast<char*>(&header_value), chunk_header_size);
        if (ptr != reinterpret_cast<char*>(&header_value)) {
            std::copy_n(ptr, sizeof(uint32_t), reinterpret_cast<char*>(&header_value));
        }
        return le_to_cpu(header_value);
    };

    if (src) {
        auto header = read_le<uint32_t>(src->get());
        if (header & last_chunk_flag) {
            // faster path for small messages: single chunk in a single buffer
            header &= ~last_chunk_flag;
            src_offset += chunk_header_size;
            src_left -= chunk_header_size;
            auto dst = temporary_buffer<char>(header);
            if (LZ4_decompress_safe_continue(stream.get(), src->get() + src_offset, dst.get_write(), src_left, header) < 0) {
                throw std::runtime_error("RPC frame LZ4_FRAGMENTED decompression failure (short)");
            }
            return rcv_buf(std::move(dst));
        }
        // not eligible for fast path: multiple chunks in a single buffer
    } else {
        // not eligible for fast path: multiple buffers
        src = std::get<std::vector<temporary_buffer<char>>>(data.bufs).data();
    }

    // Let's be a bit paranoid and not assume that the remote has the same
    // LZ4_COMPRESSBOUND as we do and allow any compressed chunk size.
    static thread_local auto chunk_buffer = temporary_buffer<char>(LZ4_COMPRESSBOUND(chunk_size));

    std::vector<temporary_buffer<char>> dst_buffers;
    size_t total_size = 0;

    // Decompressing requires either dest to be fully split or
    // "preserved" in 64KB or larger, depending on how it was
    // compressed. If not, decompression will fail, typically
    // on text-like constructs. Making our dest buffers 64K
    // ensures we retain a suitable dictionary region for all
    // passes.
    constexpr auto buf_size = 64 * 1024;
    size_t dst_offset = 0;

    auto get_dest = [&](size_t size) {
        if (dst_buffers.empty()) {
            dst_buffers.emplace_back(buf_size);
        }
        if (dst_buffers.back().size() - dst_offset < size) {
            dst_buffers.back().trim(dst_offset);
            dst_buffers.emplace_back(buf_size);
            dst_offset = 0;
        }
        return dst_buffers.back().get_write() + std::exchange(dst_offset, dst_offset + size);
    };

    // Intermediate chunks
    uint32_t header_value = read_header();
    while (!(header_value & last_chunk_flag)) {
        total_size += chunk_size;
        if (chunk_buffer.size() < header_value) {
            chunk_buffer = temporary_buffer<char>(header_value);
        }
        auto src_ptr = copy_src(chunk_buffer.get_write(), header_value);
        auto dst = get_dest(chunk_size);
        if (LZ4_decompress_safe_continue(stream.get(), src_ptr, /*dst_buffers.back().get_write()*/dst, header_value, chunk_size) < 0) {
            throw std::runtime_error(format("RPC frame LZ4_FRAGMENTED decompression failure (long, at {} bytes)", total_size - chunk_size));
        }
        header_value = read_header();
    }

    // Last chunk
    header_value &= ~last_chunk_flag;
    total_size += header_value;
    auto dst = get_dest(header_value);
    if (chunk_buffer.size() < src_left) {
        chunk_buffer = temporary_buffer<char>(src_left);
    }
    auto last_chunk_compressed_size = src_left;
    auto src_ptr = copy_src(chunk_buffer.get_write(), src_left);
    if (LZ4_decompress_safe_continue(stream.get(), src_ptr, /*dst_buffers.back().get_write()*/dst, last_chunk_compressed_size, header_value) < 0) {
        throw std::runtime_error(format("RPC frame LZ4_FRAGMENTED decompression failure (long, last frame, at {} bytes)", total_size - header_value));
    }

    dst_buffers.back().trim(dst_offset);

    if (dst_buffers.size() == 1) {
        return rcv_buf(std::move(dst_buffers.front()));
    }
    return rcv_buf(std::move(dst_buffers), total_size);
}

}
}
