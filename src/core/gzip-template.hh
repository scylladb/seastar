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

#include <seastar/core/temporary_buffer.hh>
#include <seastar/util/noncopyable_function.hh>

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace seastar::internal {

/// \brief Updates a gzip (IEEE 802.3) CRC-32 with \c n bytes at \c p.
///
/// Compatible with zlib's crc32(): start with \c crc = 0.
uint32_t crc32_update(uint32_t crc, const char* p, size_t n) noexcept;

/// \brief Returns an operator that appends \c len bytes to a CRC-32.
///
/// Equivalent to zlib's crc32_combine_gen(); the result is consumed by
/// crc32_combine_op().
uint32_t crc32_combine_gen(uint64_t len) noexcept;

/// \brief Computes the CRC-32 of the concatenation of two byte sequences.
///
/// Given \c crc1, the CRC-32 of the first sequence, \c crc2, the CRC-32 of
/// the second, and \c op, the result of crc32_combine_gen() for the length of
/// the second, returns the CRC-32 of both sequences concatenated. Equivalent
/// to zlib's crc32_combine_op().
uint32_t crc32_combine_op(uint32_t crc1, uint32_t crc2, uint32_t op) noexcept;

class gzip_template;

/// For tests: when \c enable is false, gzip_template doesn't use
/// carry-less multiplication instructions even if available.
void gzip_template_use_clmul(bool enable) noexcept;

/// \brief Builds a \ref gzip_template.
///
/// The uncompressed stream is described as a sequence of literal byte runs
/// and fixed-width holes, whose content is only supplied when rendering.
class gzip_template_builder {
    // Deques, which keep the allocations small however large the stream is
    std::deque<uint8_t> _data;
    struct hole {
        size_t offset;
        size_t width;
    };
    std::deque<hole> _holes;
    uint32_t _zeroed_crc = 0; // CRC-32 of the stream, with zeroed holes
public:
    /// The maximum width of a single hole.
    static constexpr size_t max_hole_width = 1024;

    /// Appends literal bytes to the stream.
    void append(std::string_view literal) {
        _data.insert(_data.end(), literal.begin(), literal.end());
        _zeroed_crc = crc32_update(_zeroed_crc, literal.data(), literal.size());
    }

    /// Appends a hole of \c width bytes to the stream. The content of the hole
    /// is supplied to gzip_template::render(). \c width must not exceed
    /// \ref max_hole_width.
    void append_hole(size_t width);

    /// Compresses the stream, consuming the builder. \c maybe_yield is
    /// called periodically, to allow preemption of long compressions.
    gzip_template build(noncopyable_function<void()> maybe_yield = [] {}) &&;
};

/// \brief A pre-compressed gzip stream with holes.
///
/// The stream is compressed once, with the holes left blank. Rendering
/// copies the compressed stream, encodes the holes' content into it and
/// computes the gzip trailer, which is far cheaper than compressing.
///
/// To make this possible the stream is a single deflate block using the
/// fixed Huffman code (RFC 1951, 3.2.6), in which every byte below 0x90
/// is encoded as an 8-bit literal code. Since the holes have a fixed width,
/// their position in the compressed stream is fixed, and so are the distances
/// of LZ77 back-references that span them. Back-references never refer to
/// hole content.
class gzip_template {
    struct hole {
        uint64_t bit_offset;     // in the compressed stream
        uint32_t width;
        uint32_t first_constant; // index into _crc_constants
    };
    // Deques, which keep the allocations small however large the stream is
    // The header and deflate stream, holes zeroed, in chunks of buffer_size
    // bytes (except the last), one for each buffer render() returns
    std::vector<std::vector<uint8_t>> _data;
    std::deque<hole> _holes;
    // The CRC-32 of the uncompressed stream is linear in the holes' content,
    // so it's the CRC-32 of the stream with zeroed holes, XORed with the
    // contribution of each hole's content. A hole's content is split into
    // chunks of up to 8 bytes, from its end, and the contribution of a
    // chunk C followed by n bytes is C(x) * x^(32 + 8n) modulo the CRC
    // polynomial. _crc_constants holds x^(8n) modulo the polynomial for each
    // chunk, bit-reflected (see crc32_combine_gen()).
    std::deque<uint32_t> _crc_constants;
    uint32_t _zeroed_crc = 0;
    uint32_t _isize = 0;

    // The size of the buffers render() returns, except the last
    static constexpr size_t buffer_size = 64 * 1024;
    // Zeroed room after each buffer, for the codes of a hole which starts
    // in the buffer and continues in the next one, for 8-byte accesses
    // starting at any byte of a hole, and for the trailer
    static constexpr size_t buffer_slack = gzip_template_builder::max_hole_width + 16;
    // render() fills the holes' content in batches of up to this size
    static constexpr size_t batch_size = 4096;
    static_assert(batch_size >= gzip_template_builder::max_hole_width);

    // Copies the stream into buffers of buffer_size bytes, each followed by
    // buffer_slack zeroed bytes.
    std::vector<temporary_buffer<char>> copy_data() const;
    using hole_iterator = std::deque<hole>::const_iterator;

    // Encodes the content of holes [begin, end), concatenated in contents
    // (which must be preceded by 8 readable bytes), into bufs, and XORs
    // their contribution into crc.
    bool encode_holes(std::vector<temporary_buffer<char>>& bufs, hole_iterator begin, hole_iterator end,
            const char* contents, uint32_t& crc) const noexcept;
    // Moves the codes which were encoded past the end of each buffer to the
    // start of the next one, appends the trailer to the last buffer, and
    // trims the buffers.
    void finish(std::vector<temporary_buffer<char>>& bufs, uint32_t crc) const noexcept;

    friend class gzip_template_builder;
public:
    /// The number of holes in the stream.
    size_t hole_count() const noexcept {
        return _holes.size();
    }

    /// The width of hole \c i.
    size_t hole_width(size_t i) const noexcept {
        return _holes[i].width;
    }

    /// The size of the compressed template, excluding the trailer.
    size_t compressed_size() const noexcept {
        return _data.empty() ? 0 : (_data.size() - 1) * buffer_size + _data.back().size();
    }

    /// \brief Renders the gzip stream.
    ///
    /// Calls \c fill(i, dst, width) for each hole in order, which must write
    /// exactly \c width bytes, each below 0x90, to \c dst and return true,
    /// or return false to abort rendering.
    ///
    /// \return the complete gzip stream, in buffers of up to 64 kB (so the
    ///         allocations are small however large the stream is), or
    ///         std::nullopt if \c fill failed or wrote a byte that can't be
    ///         encoded.
    template <typename Fill>
    std::optional<std::vector<temporary_buffer<char>>> render(Fill&& fill) const;
};

template <typename Fill>
std::optional<std::vector<temporary_buffer<char>>> gzip_template::render(Fill&& fill) const {
    auto bufs = copy_data();
    uint32_t crc = _zeroed_crc;
    // The content of a batch of holes, preceded by 8 bytes so that
    // encode_holes() can load 8-byte words ending at any hole
    std::array<char, 8 + batch_size> contents;
    std::fill_n(contents.begin(), 8, 0);
    char* dst = contents.data() + 8;
    auto first = _holes.begin();
    size_t i = 0;
    for (auto h = _holes.begin(); h != _holes.end(); ++h, ++i) {
        auto width = size_t(h->width);
        if (size_t(contents.data() + contents.size() - dst) < width) {
            if (!encode_holes(bufs, first, h, contents.data() + 8, crc)) {
                return std::nullopt;
            }
            first = h;
            dst = contents.data() + 8;
        }
        if (!fill(i, dst, width)) {
            return std::nullopt;
        }
        dst += width;
    }
    if (!encode_holes(bufs, first, _holes.end(), contents.data() + 8, crc)) {
        return std::nullopt;
    }
    finish(bufs, crc);
    return bufs;
}

}
