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

#include "gzip-template.hh"

#include <seastar/util/assert.hh>

#include <algorithm>
#include <atomic>
#include <bit>
#include <deque>
#include <limits>
#include <stdexcept>

#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#include <sys/auxv.h>
#endif

namespace seastar::internal {

namespace {

constexpr uint32_t crc_poly = 0xedb88320; // reflected IEEE 802.3 polynomial

constexpr auto crc_table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) {
            c = c & 1 ? (c >> 1) ^ crc_poly : c >> 1;
        }
        t[n] = c;
    }
    return t;
}();

// Multiplies a(x) by b(x) modulo the CRC polynomial (bit-reflected).
constexpr uint32_t multmodp(uint32_t a, uint32_t b) noexcept {
    uint32_t m = uint32_t(1) << 31;
    uint32_t p = 0;
    for (;;) {
        if (a & m) {
            p ^= b;
            if ((a & (m - 1)) == 0) {
                break;
            }
        }
        m >>= 1;
        b = b & 1 ? (b >> 1) ^ crc_poly : b >> 1;
    }
    return p;
}

// x2n_table[k] = x^(2^k) modulo the CRC polynomial
constexpr auto x2n_table = [] {
    std::array<uint32_t, 32> t{};
    uint32_t p = uint32_t(1) << 30; // x^1
    t[0] = p;
    for (unsigned n = 1; n < 32; ++n) {
        t[n] = p = multmodp(p, p);
    }
    return t;
}();

constexpr unsigned reverse_bits(unsigned v, unsigned nbits) noexcept {
    unsigned r = 0;
    for (unsigned i = 0; i < nbits; ++i) {
        r = (r << 1) | ((v >> i) & 1);
    }
    return r;
}

// Deflate parameters (RFC 1951)
constexpr size_t window_size = 32768;
constexpr size_t min_match = 3;
constexpr size_t max_match = 258;
// A minimum-length match this far away costs more than three literals
// with the fixed Huffman code.
constexpr size_t too_far = 4096;

constexpr std::array<uint16_t, 29> length_base = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<uint8_t, 29> length_extra = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<uint16_t, 30> dist_base = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<uint8_t, 30> dist_extra = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Writes bits to chunks of chunk_size bytes
class bit_writer {
    std::vector<std::vector<uint8_t>>& _out;
    size_t _chunk_size;
    uint64_t _size = 0;
    uint64_t _acc = 0;
    unsigned _nbits = 0;

    void push(uint8_t b) {
        if (_out.empty() || _out.back().size() == _chunk_size) {
            _out.emplace_back().reserve(_chunk_size);
        }
        _out.back().push_back(b);
        ++_size;
    }
public:
    bit_writer(std::vector<std::vector<uint8_t>>& out, size_t chunk_size) : _out(out), _chunk_size(chunk_size) {}

    // Appends the low nbits of v, least significant bit first.
    void put(uint32_t v, unsigned nbits) {
        _acc |= uint64_t(v) << _nbits;
        _nbits += nbits;
        while (_nbits >= 8) {
            push(_acc & 0xff);
            _acc >>= 8;
            _nbits -= 8;
        }
    }

    // Appends a Huffman code, most significant bit first.
    void put_code(uint32_t code, unsigned nbits) {
        put(reverse_bits(code, nbits), nbits);
    }

    uint64_t position() const noexcept {
        return _size * 8 + _nbits;
    }

    void flush() {
        if (_nbits) {
            push(_acc & 0xff);
            _acc = 0;
            _nbits = 0;
        }
    }
};

// Emits symbols using the fixed Huffman code (RFC 1951, 3.2.6)
class fixed_huffman_encoder {
    bit_writer& _w;

    void put_litlen(unsigned sym) {
        if (sym < 144) {
            _w.put_code(0x30 + sym, 8);
        } else if (sym < 256) {
            _w.put_code(0x190 + sym - 144, 9);
        } else if (sym < 280) {
            _w.put_code(sym - 256, 7);
        } else {
            _w.put_code(0xc0 + sym - 280, 8);
        }
    }
public:
    explicit fixed_huffman_encoder(bit_writer& w) : _w(w) {}

    void begin_final_block() {
        _w.put(1, 1); // BFINAL
        _w.put(1, 2); // BTYPE = fixed Huffman
    }

    void literal(uint8_t c) {
        put_litlen(c);
    }

    void match(size_t len, size_t dist) {
        auto li = std::upper_bound(length_base.begin(), length_base.end(), len) - length_base.begin() - 1;
        put_litlen(257 + li);
        _w.put(len - length_base[li], length_extra[li]);
        auto di = std::upper_bound(dist_base.begin(), dist_base.end(), dist) - dist_base.begin() - 1;
        _w.put_code(di, 5);
        _w.put(dist - dist_base[di], dist_extra[di]);
    }

    void end_block() {
        put_litlen(256);
    }
};

// Which bytes of the uncompressed stream are in holes, in a deque to keep
// the allocations small
class hole_bitmap {
    std::deque<uint64_t> _bits;
public:
    explicit hole_bitmap(size_t n) : _bits((n + 63) / 64) {}

    void set(size_t p) noexcept {
        _bits[p / 64] |= uint64_t(1) << (p % 64);
    }

    // Bits [64 * i, 64 * i + 64), zero beyond the end
    uint64_t word(size_t i) const noexcept {
        return i < _bits.size() ? _bits[i] : 0;
    }
};

// A contiguous copy of the part of the uncompressed stream, and of its hole
// bitmap, which the compressor accesses around a position: the deflate
// window behind it and the longest match ahead of it. It keeps the hot
// loops away from the deques.
class stream_window {
    static constexpr size_t capacity = 3 * window_size;
    const std::deque<uint8_t>& _data;
    const hole_bitmap& _holes;
    size_t _base = 0; // position of the window's first byte
    std::vector<uint8_t> _bytes;
    std::vector<uint64_t> _hole_bits;

    // Fills the window from offset from (a multiple of 64)
    void fill(size_t from) {
        auto end = std::min(_bytes.size(), _data.size() - _base);
        auto p = std::copy(_data.begin() + _base + from, _data.begin() + _base + end, _bytes.begin() + from);
        std::fill(p, _bytes.end(), 0);
        for (size_t i = from / 64; i < _hole_bits.size(); ++i) {
            _hole_bits[i] = _holes.word(_base / 64 + i);
        }
    }
public:
    stream_window(const std::deque<uint8_t>& data, const hole_bitmap& holes)
            : _data(data), _holes(holes), _bytes(std::min(data.size(), capacity)), _hole_bits((_bytes.size() + 63) / 64) {
        fill(0);
    }

    // Makes the bytes from window_size before p to max_match + 3 after it
    // accessible, for positions p which never decrease.
    void advance(size_t p) {
        if (p + max_match + 3 > _base + _bytes.size() && _base + _bytes.size() < _data.size()) {
            _base += window_size;
            std::copy(_bytes.begin() + window_size, _bytes.end(), _bytes.begin());
            std::copy(_hole_bits.begin() + window_size / 64, _hole_bits.end(), _hole_bits.begin());
            fill(_bytes.size() - window_size);
        }
    }

    uint8_t operator[](size_t p) const noexcept {
        return _bytes[p - _base];
    }

    bool is_hole(size_t p) const noexcept {
        auto i = p - _base;
        return (_hole_bits[i / 64] >> (i % 64)) & 1;
    }
};

// LZ77 match finder using hash chains, restricted to bytes outside holes.
class match_finder {
    static constexpr unsigned hash_bits = 15;
    static constexpr unsigned max_chain = 128;
    static constexpr size_t nice_match = 258;

    const stream_window& _d;
    size_t _n;
    // 32-bit positions keep the tables at 128 kB
    std::vector<int32_t> _head;
    std::vector<int32_t> _prev;

    unsigned hash(size_t p) const noexcept {
        uint32_t v = uint32_t(_d[p]) | uint32_t(_d[p + 1]) << 8 | uint32_t(_d[p + 2]) << 16;
        return (v * 2654435761u) >> (32 - hash_bits);
    }
    bool hashable(size_t p) const noexcept {
        return p + min_match <= _n && !_d.is_hole(p) && !_d.is_hole(p + 1) && !_d.is_hole(p + 2);
    }
public:
    match_finder(const stream_window& d, size_t n)
        : _d(d), _n(n), _head(size_t(1) << hash_bits, -1), _prev(window_size, -1) {
    }

    void insert(size_t p) {
        if (hashable(p)) {
            auto h = hash(p);
            _prev[p % window_size] = _head[h];
            _head[h] = p;
        }
    }

    struct match {
        size_t len = 0;
        size_t dist = 0;
    };

    // Finds the longest match for position p, of at most limit bytes (all
    // outside holes).
    match find(size_t p, size_t limit) const {
        match best;
        if (limit < min_match || !hashable(p)) {
            return best;
        }
        auto cur = _head[hash(p)];
        for (unsigned chain = max_chain; cur >= 0 && chain; --chain) {
            size_t c = cur;
            if (p - c > window_size) {
                break;
            }
            // best.len < limit here, so this stays within the run
            if (_d[c + best.len] == _d[p + best.len]) {
                size_t len = 0;
                while (len < limit && _d[c + len] == _d[p + len] && !_d.is_hole(c + len)) {
                    ++len;
                }
                if (len > best.len && (len > min_match || p - c <= too_far)) {
                    best = {len, p - c};
                    if (len >= std::min(limit, nice_match)) {
                        break;
                    }
                }
            }
            auto next = _prev[c % window_size];
            if (next >= cur) {
                break; // overwritten by a newer position
            }
            cur = next;
        }
        if (best.len < min_match) {
            best = {};
        }
        return best;
    }
};

}

namespace {

// The fixed Huffman codes of the literals encodable in holes, bit-reversed
constexpr auto literal_codes = [] {
    std::array<uint8_t, 0x90> t{};
    for (unsigned c = 0; c < t.size(); ++c) {
        t[c] = reverse_bits(0x30 + c, 8);
    }
    return t;
}();

uint64_t load_le64(const void* p) noexcept {
    uint64_t v;
    std::memcpy(&v, p, 8);
    if constexpr (std::endian::native == std::endian::big) {
        v = __builtin_bswap64(v);
    }
    return v;
}

void store_le64(void* p, uint64_t v) noexcept {
    if constexpr (std::endian::native == std::endian::big) {
        v = __builtin_bswap64(v);
    }
    std::memcpy(p, &v, 8);
}

// Encodes a hole's content as literal codes at a bit offset of out, which
// must be followed by at least 8 bytes. Returns false if a byte can't be
// encoded.
bool encode_hole(uint8_t* out, uint64_t bit_offset, const char* src, uint32_t width) noexcept {
    auto* dst = out + bit_offset / 8;
    unsigned shift = bit_offset % 8;
    // Seven codes at a time: shifted by up to 7 bits, they fit in a word
    for (uint32_t i = 0; i < width; i += 7) {
        uint64_t codes = 0;
        uint32_t n = std::min(width - i, 7u);
        for (uint32_t k = 0; k < n; ++k) {
            auto c = static_cast<uint8_t>(src[i + k]);
            if (c >= literal_codes.size()) [[unlikely]] {
                return false;
            }
            codes |= uint64_t(literal_codes[c]) << (8 * k);
        }
        store_le64(dst + i, load_le64(dst + i) | (codes << shift));
    }
    return true;
}

// Loads the n <= 8 bytes of a chunk ending at end (which must be preceded by
// 8 - n readable bytes) as a bit-reflected polynomial: x^0 at bit 63.
uint64_t load_chunk(const char* end, unsigned n) noexcept {
    auto v = load_le64(end - 8);
    return n == 8 ? v : v & (~uint64_t(0) << (64 - 8 * n));
}

// Iterates over the chunks of all holes, from the end of each hole
#define SEASTAR_FOR_EACH_HOLE_CHUNK(holes, contents, constants, chunk, k, body) \
    for (auto& h_ : holes) { \
        contents += h_.width; \
        auto k_ = constants + h_.first_constant; \
        for (uint32_t done_ = 0; done_ < h_.width; done_ += 8) { \
            uint64_t chunk = load_chunk(contents - done_, std::min(h_.width - done_, 8u)); \
            uint32_t k = *k_++; \
            body \
        } \
    }

// Reduces the carry-less product of chunks (bit-reflected, with x^0 at bit
// 94) and their constants, multiplied by x^32, modulo the CRC polynomial.
uint32_t reduce_product(uint64_t lo, uint64_t hi) noexcept {
    static const uint32_t x32 = crc32_combine_gen(4);
    static const uint32_t x64 = crc32_combine_gen(8);
    static const uint32_t x96 = crc32_combine_gen(12);
    // Bit 63 + k, 31 + k and k - 1 of the product have x^(31 - k) times
    // x^-32, x^0 and x^32, respectively.
    return multmodp(x32, uint32_t((lo >> 63) | (hi << 1)))
        ^ multmodp(x64, uint32_t(lo >> 31))
        ^ multmodp(x96, uint32_t(lo << 1));
}

#if defined(__x86_64__)

[[gnu::target("pclmul")]]
uint32_t holes_crc_clmul(const auto& holes, const char* contents, auto constants) noexcept {
    auto acc = _mm_setzero_si128();
    SEASTAR_FOR_EACH_HOLE_CHUNK(holes, contents, constants, chunk, k, {
        acc = _mm_xor_si128(acc, _mm_clmulepi64_si128(_mm_cvtsi64_si128(chunk), _mm_cvtsi32_si128(int(k)), 0));
    })
    return reduce_product(_mm_cvtsi128_si64(acc), _mm_cvtsi128_si64(_mm_unpackhi_epi64(acc, acc)));
}

bool have_clmul() noexcept {
    static const bool have = __builtin_cpu_supports("pclmul");
    return have;
}

#elif defined(__aarch64__)

[[gnu::target("+aes")]]
uint32_t holes_crc_clmul(const auto& holes, const char* contents, auto constants) noexcept {
    auto acc = vdupq_n_u64(0);
    SEASTAR_FOR_EACH_HOLE_CHUNK(holes, contents, constants, chunk, k, {
        acc = veorq_u64(acc, vreinterpretq_u64_p128(vmull_p64(chunk, k)));
    })
    return reduce_product(vgetq_lane_u64(acc, 0), vgetq_lane_u64(acc, 1));
}

bool have_clmul() noexcept {
    static const bool have = getauxval(AT_HWCAP) & HWCAP_PMULL;
    return have;
}

#endif

uint32_t holes_crc_generic(const auto& holes, const char* contents, auto constants) noexcept {
    uint32_t acc = 0;
    SEASTAR_FOR_EACH_HOLE_CHUNK(holes, contents, constants, chunk, k, {
        // The CRC of the chunk bytes, without pre- and post-conditioning,
        // is the chunk multiplied by x^32 modulo the CRC polynomial
        uint32_t r = 0;
        for (unsigned i = 0; i < 8; ++i) {
            r = crc_table[(r ^ (chunk >> (8 * i))) & 0xff] ^ (r >> 8);
        }
        acc ^= multmodp(k, r);
    })
    return acc;
}

#undef SEASTAR_FOR_EACH_HOLE_CHUNK

std::atomic<bool> use_clmul = true;

}

void gzip_template_use_clmul(bool enable) noexcept {
    use_clmul.store(enable, std::memory_order_relaxed);
}

std::vector<temporary_buffer<char>> gzip_template::copy_data() const {
    std::vector<temporary_buffer<char>> bufs;
    for (auto& chunk : _data) {
        temporary_buffer<char> buf(chunk.size() + buffer_slack);
        auto* p = std::copy(chunk.begin(), chunk.end(), buf.get_write());
        std::fill_n(p, buffer_slack, 0);
        bufs.push_back(std::move(buf));
    }
    return bufs;
}

bool gzip_template::encode_holes(std::vector<temporary_buffer<char>>& bufs, hole_iterator begin, hole_iterator end,
        const char* contents, uint32_t& crc) const noexcept {
    const char* p = contents;
    for (auto& h : std::ranges::subrange(begin, end)) {
        // The hole is encoded into the buffer it starts in, continuing
        // into its slack.
        auto buf = h.bit_offset / 8 / buffer_size;
        auto* out = reinterpret_cast<uint8_t*>(bufs[buf].get_write());
        if (!encode_hole(out, h.bit_offset - buf * buffer_size * 8, p, h.width)) {
            return false;
        }
        p += h.width;
    }
    auto holes = std::ranges::subrange(begin, end);
#if defined(__x86_64__) || defined(__aarch64__)
    if (use_clmul && have_clmul()) [[likely]] {
        crc ^= holes_crc_clmul(holes, contents, _crc_constants.begin());
    } else
#endif
    {
        crc ^= holes_crc_generic(holes, contents, _crc_constants.begin());
    }
    return true;
}

void gzip_template::finish(std::vector<temporary_buffer<char>>& bufs, uint32_t crc) const noexcept {
    for (size_t i = 0; i < bufs.size(); ++i) {
        auto len = _data[i].size();
        if (i + 1 < bufs.size()) {
            // A hole continues into the next buffer at most by its width
            // (plus a byte, since codes needn't be byte aligned), and the
            // stream always continues past a hole (at least with the
            // end-of-block code), so the codes are within the next buffer.
            auto* slack = bufs[i].get_write() + len;
            auto& next = bufs[i + 1];
            auto n = std::min(buffer_slack, next.size() - buffer_slack);
            for (size_t j = 0; j < n; ++j) {
                next.get_write()[j] |= slack[j];
            }
            bufs[i].trim(len);
        } else {
            auto* trailer = bufs[i].get_write() + len;
            for (unsigned j = 0; j < 4; ++j) {
                trailer[j] = crc >> (8 * j);
                trailer[4 + j] = _isize >> (8 * j);
            }
            bufs[i].trim(len + 8);
        }
    }
}

uint32_t crc32_update(uint32_t crc, const char* p, size_t n) noexcept {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc = crc_table[(crc ^ static_cast<uint8_t>(p[i])) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}

uint32_t crc32_combine_gen(uint64_t len) noexcept {
    // x^(8 * len) modulo the CRC polynomial
    uint32_t p = uint32_t(1) << 31; // x^0
    for (unsigned k = 3; len; len >>= 1, ++k) {
        if (len & 1) {
            p = multmodp(x2n_table[k & 31], p);
        }
    }
    return p;
}

uint32_t crc32_combine_op(uint32_t crc1, uint32_t crc2, uint32_t op) noexcept {
    return multmodp(op, crc1) ^ crc2;
}

void gzip_template_builder::append_hole(size_t width) {
    if (width > max_hole_width) {
        throw std::length_error("gzip template hole too wide");
    }
    _holes.push_back({_data.size(), width});
    // The content is irrelevant, as it is never referenced by matches.
    _data.insert(_data.end(), width, 0);
    static constexpr std::array<char, max_hole_width> zeros{};
    _zeroed_crc = crc32_update(_zeroed_crc, zeros.data(), width);
}

gzip_template gzip_template_builder::build(noncopyable_function<void()> maybe_yield) && {
    gzip_template t;
    const size_t n = _data.size();
    if (n > size_t(std::numeric_limits<int32_t>::max())) {
        throw std::length_error("gzip template too large");
    }

    hole_bitmap is_hole(n);
    for (auto& h : _holes) {
        for (size_t k = 0; k < h.width; ++k) {
            is_hole.set(h.offset + k);
        }
    }

    bit_writer w(t._data, gzip_template::buffer_size);
    // RFC 1952 header: no flags, no mtime, unknown OS
    for (uint8_t b : {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff}) {
        w.put(b, 8);
    }
    fixed_huffman_encoder enc(w);
    stream_window win(_data, is_hole);
    match_finder mf(win, n);

    enc.begin_final_block();
    size_t p = 0;
    constexpr size_t yield_interval = 64 * 1024;
    size_t next_yield = yield_interval;
    for (size_t hi = 0; hi <= _holes.size(); ++hi) {
        const size_t run_end = hi < _holes.size() ? _holes[hi].offset : n;
        // Compress the literal run [p, run_end), with lazy matching.
        while (p < run_end) {
            win.advance(p);
            if (p >= next_yield) {
                maybe_yield();
                next_yield = p + yield_interval;
            }
            auto m = mf.find(p, std::min(max_match, run_end - p));
            mf.insert(p);
            if (m.len && m.len < 32 && p + 1 < run_end) {
                auto next = mf.find(p + 1, std::min(max_match, run_end - p - 1));
                if (next.len > m.len) {
                    enc.literal(win[p]);
                    ++p;
                    continue;
                }
            }
            if (m.len) {
                enc.match(m.len, m.dist);
                for (size_t k = 1; k < m.len; ++k) {
                    mf.insert(p + k);
                }
                p += m.len;
            } else {
                enc.literal(win[p]);
                ++p;
            }
        }
        if (hi == _holes.size()) {
            break;
        }
        const auto& h = _holes[hi];
        t._holes.push_back({w.position(), uint32_t(h.width), uint32_t(t._crc_constants.size())});
        // The chunks of the hole, from its end, and the bytes following them
        const size_t after = n - (h.offset + h.width);
        for (size_t chunk_end = h.width; chunk_end > 0; chunk_end -= std::min<size_t>(chunk_end, 8)) {
            t._crc_constants.push_back(crc32_combine_gen(after + h.width - chunk_end));
        }
        for (size_t k = 0; k < h.width; ++k) {
            w.put(0, 8);
        }
        p = h.offset + h.width;
    }
    enc.end_block();
    w.flush();
    t._zeroed_crc = _zeroed_crc;
    t._isize = uint32_t(n);
    // The template is kept for a while, so release the unused capacity
    if (!t._data.empty()) {
        t._data.back().shrink_to_fit();
    }
    t._data.shrink_to_fit();
    t._holes.shrink_to_fit();
    t._crc_constants.shrink_to_fit();
    return t;
}

}
