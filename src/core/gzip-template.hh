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

/// \brief Builds a \ref gzip_template.
///
/// The uncompressed stream is described as a sequence of literal byte runs
/// and fixed-width holes, whose content is only supplied when rendering.
class gzip_template_builder {
    std::string _data;
    struct hole {
        size_t offset;
        size_t width;
    };
    std::vector<hole> _holes;
public:
    /// The maximum width of a single hole.
    static constexpr size_t max_hole_width = 1024;

    /// Appends literal bytes to the stream.
    void append(std::string_view literal) {
        _data.append(literal);
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
        uint64_t bit_offset;  // in the compressed stream
        uint32_t width;
        uint32_t crc_before;  // CRC-32 of the literal run preceding the hole
        uint32_t op_before;   // crc32_combine_gen() of that run's length
    };
    std::vector<uint8_t> _data; // header and deflate stream, holes zeroed
    std::vector<hole> _holes;
    uint32_t _tail_crc = 0;
    uint32_t _tail_op = 0;
    uint32_t _isize = 0;

    static const std::array<uint8_t, 0x90> literal_codes;

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
        return _data.size();
    }

    /// \brief Renders the gzip stream.
    ///
    /// Calls \c fill(i, dst, width) for each hole in order, which must write
    /// exactly \c width bytes, each below 0x90, to \c dst and return true,
    /// or return false to abort rendering.
    ///
    /// \return the complete gzip stream, or std::nullopt if \c fill failed or
    ///         wrote a byte that can't be encoded.
    template <typename Fill>
    std::optional<temporary_buffer<char>> render(Fill&& fill) const;
};

template <typename Fill>
std::optional<temporary_buffer<char>> gzip_template::render(Fill&& fill) const {
    temporary_buffer<char> out(_data.size() + 8);
    auto* p = reinterpret_cast<uint8_t*>(out.get_write());
    std::memcpy(p, _data.data(), _data.size());
    uint32_t crc = 0;
    char tmp[gzip_template_builder::max_hole_width];
    for (size_t i = 0; i < _holes.size(); ++i) {
        const auto& h = _holes[i];
        crc = crc32_combine_op(crc, h.crc_before, h.op_before);
        if (!fill(i, tmp, size_t(h.width))) {
            return std::nullopt;
        }
        crc = crc32_update(crc, tmp, h.width);
        // Each byte is an 8-bit Huffman code, which may straddle two bytes
        // of the compressed stream.
        auto* dst = p + h.bit_offset / 8;
        unsigned shift = h.bit_offset % 8;
        for (uint32_t j = 0; j < h.width; ++j) {
            auto c = static_cast<unsigned char>(tmp[j]);
            if (c >= literal_codes.size()) [[unlikely]] {
                return std::nullopt;
            }
            unsigned code = unsigned(literal_codes[c]) << shift;
            dst[j] |= code;
            // The stream always continues past a hole (at least with the
            // end-of-block code), so dst[j + 1] is in bounds.
            dst[j + 1] |= code >> 8;
        }
    }
    crc = crc32_combine_op(crc, _tail_crc, _tail_op);
    auto* trailer = p + _data.size();
    for (unsigned i = 0; i < 4; ++i) {
        trailer[i] = crc >> (8 * i);
        trailer[4 + i] = _isize >> (8 * i);
    }
    return out;
}

}
