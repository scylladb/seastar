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

#define BOOST_TEST_MODULE gzip_template

#include "core/gzip-template.hh"
#include "gunzip.hh"

#include <boost/test/unit_test.hpp>
#include <zlib.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <random>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace seastar;
using namespace seastar::internal;

BOOST_AUTO_TEST_CASE(test_crc32) {
    std::mt19937 rng(1);
    for (size_t len : {0, 1, 7, 100, 4096, 100000}) {
        std::string s(len, 0);
        for (auto& c : s) {
            c = rng();
        }
        for (size_t split : {size_t(0), len / 3, len}) {
            auto a = std::string_view(s).substr(0, split);
            auto b = std::string_view(s).substr(split);
            auto crc_a = crc32_update(0, a.data(), a.size());
            auto crc_b = crc32_update(0, b.data(), b.size());
            auto expected = ::crc32(0, reinterpret_cast<const Bytef*>(s.data()), s.size());
            BOOST_REQUIRE_EQUAL(crc32_update(crc_a, b.data(), b.size()), expected);
            BOOST_REQUIRE_EQUAL(crc32_combine_op(crc_a, crc_b, crc32_combine_gen(b.size())), expected);
        }
    }
}

// Builds a random template resembling metrics text, renders it with random
// hole content, and verifies that zlib decompresses it to the expected text.
static void test_random_template(unsigned seed, size_t lines) {
    std::mt19937 rng(seed);
    std::vector<std::string> names = {"seastar_reactor_utilization", "seastar_memory_allocated_memory", "x"};
    gzip_template_builder b;
    // The stream as literals, and holes (as their index in widths)
    std::vector<std::variant<std::string, size_t>> pieces;
    std::vector<size_t> widths;
    auto literal = [&] (std::string lit) {
        b.append(lit);
        pieces.emplace_back(std::move(lit));
    };
    for (size_t i = 0; i < lines; ++i) {
        literal(names[rng() % names.size()] + "{shard=\"" + std::to_string(rng() % 64) + "\"} ");
        if (rng() % 8 == 0) {
            std::string junk(rng() % 300, 0);
            for (auto& c : junk) {
                c = rng(); // any bytes
            }
            literal(std::move(junk));
        }
        for (unsigned h = rng() % 3; h; --h) {
            size_t w = rng() % 4 == 0 ? 0 : 1 + rng() % 30;
            b.append_hole(w);
            pieces.emplace_back(widths.size());
            widths.push_back(w);
        }
        literal("\n");
    }
    auto t = std::move(b).build();
    BOOST_REQUIRE_EQUAL(t.hole_count(), widths.size());

    for (int round = 0; round < 3; ++round) {
        std::vector<std::string> contents;
        for (auto w : widths) {
            std::string c(w, 0);
            for (auto& ch : c) {
                ch = char(rng() % 0x90);
            }
            contents.push_back(std::move(c));
        }
        std::string expected;
        for (auto& piece : pieces) {
            if (auto* lit = std::get_if<std::string>(&piece)) {
                expected += *lit;
            } else {
                expected += contents[std::get<size_t>(piece)];
            }
        }
        auto out = t.render([&] (size_t i, char* dst, size_t width) {
            BOOST_REQUIRE_EQUAL(width, widths[i]);
            std::copy(contents[i].begin(), contents[i].end(), dst);
            return true;
        });
        BOOST_REQUIRE(out);
        auto trailer = reinterpret_cast<const uint8_t*>(out->get() + out->size() - 8);
        uint32_t crc = trailer[0] | trailer[1] << 8 | trailer[2] << 16 | uint32_t(trailer[3]) << 24;
        BOOST_REQUIRE_EQUAL(crc, ::crc32(0, reinterpret_cast<const Bytef*>(expected.data()), expected.size()));
        auto raw = gunzip(out->get(), out->size(), true);
        if (raw != expected) {
            auto m = std::ranges::mismatch(raw, expected);
            auto pos = m.in1 - raw.begin();
            BOOST_FAIL(fmt::format("seed {} round {}: size {} vs {}, mismatch at {}: {:?} vs {:?}", seed, round, raw.size(), expected.size(), pos,
                raw.substr(pos > 20 ? pos - 20 : 0, 40), expected.substr(pos > 20 ? pos - 20 : 0, 40)));
        }
        BOOST_REQUIRE(gunzip(out->get(), out->size()) == expected);
    }
}

BOOST_AUTO_TEST_CASE(test_render_failure) {
    gzip_template_builder b;
    b.append("abc");
    b.append_hole(2);
    b.append("def");
    auto t = std::move(b).build();
    // Bytes from 0x90 have 9-bit codes, so can't fill a hole
    BOOST_REQUIRE(!t.render([] (size_t, char* dst, size_t) { dst[0] = 'x'; dst[1] = char(0x90); return true; }));
    BOOST_REQUIRE(!t.render([] (size_t, char*, size_t) { return false; }));
    auto out = t.render([] (size_t, char* dst, size_t) { dst[0] = 'x'; dst[1] = char(0x8f); return true; });
    BOOST_REQUIRE(out);
    BOOST_REQUIRE_EQUAL(gunzip(out->get(), out->size()), "abcx\x8f" "def");
}

BOOST_AUTO_TEST_CASE(test_empty_template) {
    auto t = gzip_template_builder().build();
    auto out = t.render([] (size_t, char*, size_t) { return true; });
    BOOST_REQUIRE(out);
    BOOST_REQUIRE_EQUAL(gunzip(out->get(), out->size()), "");
}

BOOST_AUTO_TEST_CASE(test_random_templates) {
    for (unsigned seed = 0; seed < 20; ++seed) {
        test_random_template(seed, 1 + seed * 50);
    }
    // Long enough to exceed the deflate window
    test_random_template(100, 20000);
}
