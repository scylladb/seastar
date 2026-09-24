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

/// For tests: when \c enable is false, gzip_template doesn't use
/// carry-less multiplication instructions even if available.
void gzip_template_use_clmul(bool enable) noexcept;

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
        uint64_t bit_offset;     // in the compressed stream
        uint32_t width;
        uint32_t first_constant; // index into _crc_constants
    };
    std::vector<uint8_t> _data; // header and deflate stream, holes zeroed
    std::vector<hole> _holes;
    // The CRC-32 of the uncompressed stream is linear in the holes' content,
    // so it's the CRC-32 of the stream with zeroed holes, XORed with the
    // contribution of each hole's content. A hole's content is split into
    // chunks of up to 8 bytes, from its end, and the contribution of a
    // chunk C followed by n bytes is C(x) * x^(32 + 8n) modulo the CRC
    // polynomial. _crc_constants holds x^(8n) modulo the polynomial for each
    // chunk, bit-reflected (see crc32_combine_gen()).
    std::vector<uint32_t> _crc_constants;
    uint32_t _zeroed_crc = 0;
    uint32_t _isize = 0;
    size_t _total_width = 0;

    // Encodes the holes' content, concatenated in contents, into out (a
    // copy of _data with room for the trailer and 8 more bytes), and
    // writes the trailer.
    bool finish(uint8_t* out, const char* contents) const noexcept;

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
    // The content of all holes, preceded by 8 bytes so that finish() can
    // load 8-byte words ending at any hole
    std::vector<char> contents(8 + _total_width);
    char* dst = contents.data() + 8;
    for (size_t i = 0; i < _holes.size(); ++i) {
        auto width = size_t(_holes[i].width);
        if (!fill(i, dst, width)) {
            return std::nullopt;
        }
        dst += width;
    }
    // Room for the trailer, and for finish() to access 8-byte words
    // starting at any byte of the stream
    temporary_buffer<char> out(_data.size() + 16);
    std::memcpy(out.get_write(), _data.data(), _data.size());
    if (!finish(reinterpret_cast<uint8_t*>(out.get_write()), contents.data() + 8)) {
        return std::nullopt;
    }
    out.trim(_data.size() + 8);
    return out;
}

}
