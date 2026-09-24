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
#include <stdexcept>

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

class bit_writer {
    std::vector<uint8_t>& _out;
    uint64_t _acc = 0;
    unsigned _nbits = 0;
public:
    explicit bit_writer(std::vector<uint8_t>& out) : _out(out) {}

    // Appends the low nbits of v, least significant bit first.
    void put(uint32_t v, unsigned nbits) {
        _acc |= uint64_t(v) << _nbits;
        _nbits += nbits;
        while (_nbits >= 8) {
            _out.push_back(_acc & 0xff);
            _acc >>= 8;
            _nbits -= 8;
        }
    }

    // Appends a Huffman code, most significant bit first.
    void put_code(uint32_t code, unsigned nbits) {
        put(reverse_bits(code, nbits), nbits);
    }

    uint64_t position() const noexcept {
        return uint64_t(_out.size()) * 8 + _nbits;
    }

    void flush() {
        if (_nbits) {
            _out.push_back(_acc & 0xff);
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

// LZ77 match finder using hash chains, restricted to bytes outside holes.
class match_finder {
    static constexpr unsigned hash_bits = 15;
    static constexpr unsigned max_chain = 128;
    static constexpr size_t nice_match = 258;

    const uint8_t* _d;
    size_t _n;
    const std::vector<bool>& _is_hole;
    std::vector<int64_t> _head;
    std::vector<int64_t> _prev;

    unsigned hash(size_t p) const noexcept {
        uint32_t v = uint32_t(_d[p]) | uint32_t(_d[p + 1]) << 8 | uint32_t(_d[p + 2]) << 16;
        return (v * 2654435761u) >> (32 - hash_bits);
    }
    bool hashable(size_t p) const noexcept {
        return p + min_match <= _n && !_is_hole[p] && !_is_hole[p + 1] && !_is_hole[p + 2];
    }
public:
    match_finder(const uint8_t* d, size_t n, const std::vector<bool>& is_hole)
        : _d(d), _n(n), _is_hole(is_hole), _head(size_t(1) << hash_bits, -1), _prev(window_size, -1) {
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
                while (len < limit && _d[c + len] == _d[p + len] && !_is_hole[c + len]) {
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

const std::array<uint8_t, 0x90> gzip_template::literal_codes = [] {
    std::array<uint8_t, 0x90> t{};
    for (unsigned c = 0; c < t.size(); ++c) {
        t[c] = reverse_bits(0x30 + c, 8);
    }
    return t;
}();

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
    _data.append(width, '\0');
}

gzip_template gzip_template_builder::build(noncopyable_function<void()> maybe_yield) && {
    gzip_template t;
    const auto* d = reinterpret_cast<const uint8_t*>(_data.data());
    const size_t n = _data.size();

    std::vector<bool> is_hole(n);
    for (auto& h : _holes) {
        std::fill_n(is_hole.begin() + h.offset, h.width, true);
    }

    // RFC 1952 header: no flags, no mtime, unknown OS
    t._data = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff};
    bit_writer w(t._data);
    fixed_huffman_encoder enc(w);
    match_finder mf(d, n, is_hole);

    enc.begin_final_block();
    size_t run_start = 0;
    size_t p = 0;
    constexpr size_t yield_interval = 64 * 1024;
    size_t next_yield = yield_interval;
    for (size_t hi = 0; hi <= _holes.size(); ++hi) {
        const size_t run_end = hi < _holes.size() ? _holes[hi].offset : n;
        // Compress the literal run [p, run_end), with lazy matching.
        while (p < run_end) {
            if (p >= next_yield) {
                maybe_yield();
                next_yield = p + yield_interval;
            }
            auto m = mf.find(p, std::min(max_match, run_end - p));
            mf.insert(p);
            if (m.len && m.len < 32 && p + 1 < run_end) {
                auto next = mf.find(p + 1, std::min(max_match, run_end - p - 1));
                if (next.len > m.len) {
                    enc.literal(d[p]);
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
                enc.literal(d[p]);
                ++p;
            }
        }
        auto run_crc = crc32_update(0, _data.data() + run_start, run_end - run_start);
        auto run_op = crc32_combine_gen(run_end - run_start);
        if (hi == _holes.size()) {
            t._tail_crc = run_crc;
            t._tail_op = run_op;
            break;
        }
        const auto& h = _holes[hi];
        t._holes.push_back({w.position(), uint32_t(h.width), run_crc, run_op});
        for (size_t k = 0; k < h.width; ++k) {
            w.put(0, 8);
        }
        p = run_start = h.offset + h.width;
    }
    enc.end_block();
    w.flush();
    t._isize = uint32_t(n);
    return t;
}

}
