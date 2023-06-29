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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <cstdlib>
#include <string_view>
#include <vector>
#include <fmt/format.h>
#endif
#include <seastar/util/modules.hh>

namespace seastar {

// Convert a string to a memory size, allowing binary SI
// suffixes (intentionally, even though SI suffixes are
// decimal, to follow existing usage). A string matched
// by following BNF is accetped:
//
// memory_size ::= <digit>+ <suffix>? "i"? "B"?
// suffix ::= ("k" | "K" | "M" | "G" | "T")
//
// for instance:
//
// "5" -> 5
// "4k" -> (4 << 10)
// "8Mi" -> (8 << 20)
// "7GB" -> (7 << 30)
// "1TiB" -> (1 << 40)
// anything else: exception
SEASTAR_MODULE_EXPORT
size_t parse_memory_size(std::string_view s);

class data_size {
    const size_t _value;
    friend struct fmt::formatter<seastar::data_size>;
public:
    data_size(size_t value) : _value{value} {}
};

static inline std::vector<char> string2vector(std::string_view str) {
    auto v = std::vector<char>(str.begin(), str.end());
    v.push_back('\0');
    return v;
}

}

// print data_size using IEC or SI unit annotation
// usage:
//   fmt::print("{}", 10'024); // prints "10Ki"
//   fmt::print("{:i}b", 42); // prints "42b"
//   fmt::print("{:i}B", 10'024); // prints "10KiB", IEC unit is used
//   fmt::print("{:s}B", 10'000); // prints "10kB", SI unit is used
SEASTAR_MODULE_EXPORT
template <>
struct fmt::formatter<seastar::data_size> {
    enum class prefix_type {
        SI,
        IEC,
    };
    prefix_type _prefix = prefix_type::IEC;
    constexpr auto parse(format_parse_context& ctx) {
        auto it = ctx.begin();
        auto end = ctx.end();
        if (it != end) {
            if (*it == 's') {
                _prefix = prefix_type::SI;
                ++it;
            } else if (*it == 'i') {
                _prefix = prefix_type::IEC;
                ++it;
            }
        }
        if (it != end && *it != '}') {
            ctx.on_error("invalid format");
        }
        return it;
    }
    template <typename FormatContext>
    auto format(const seastar::data_size size, FormatContext& ctx) const -> decltype(ctx.out());
};
