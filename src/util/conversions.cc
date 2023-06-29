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

#ifdef SEASTAR_MODULE
module;
#endif

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <cctype>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/util/conversions.hh>
#include <seastar/core/print.hh>
#endif

namespace seastar {

static constexpr struct {
    std::string_view suffix;
    unsigned power;
} suffixes[] = {
    {"k", 10},
    {"K", 10},
    {"M", 20},
    {"G", 30},
    {"T", 40},
};

size_t parse_memory_size(std::string_view s) {
    for (std::string_view unit : {"i", "iB", "B"}) {
        if (boost::algorithm::ends_with(s, unit)) {
            s.remove_suffix(unit.size());
            break;
        }
    }
    size_t factor = 1;
    for (auto [suffix, power] : suffixes) {
        if (boost::algorithm::ends_with(s, suffix)) {
            factor <<= power;
            s.remove_suffix(suffix.size());
            break;
        }
    }
    return boost::lexical_cast<size_t>(s) * factor;
}

}

template <typename Suffixes>
static constexpr std::pair<unsigned, std::string_view>
do_format(size_t n, Suffixes suffixes, unsigned scale) {
    size_t factor = n;
    std::string_view suffix;
    for (auto next_suffix : suffixes) {
        size_t next_factor = factor / scale;
        if (next_factor == 0) {
            break;
        }
        factor = next_factor;
        suffix = next_suffix;
    }
    return {factor, suffix};
}

template <typename FormatContext>
auto fmt::formatter<seastar::data_size>::format(seastar::data_size data_size,
                                               FormatContext& ctx) const -> decltype(ctx.out()) {
    if (_prefix == prefix_type::IEC) {
        // ISO/IEC units
        static constexpr auto suffixes = {"Ki", "Mi", "Gi", "Ti"};
        auto [n, suffix] = do_format(data_size._value, suffixes, 1024);
        return fmt::format_to(ctx.out(), "{}{}", n, suffix);
    } else {
        // SI units
        static constexpr auto suffixes = {"k", "M", "G", "T"};
        auto [n, suffix] = do_format(data_size._value, suffixes, 1000);
        return fmt::format_to(ctx.out(), "{}{}", n, suffix);
    }
}

template
auto fmt::formatter<seastar::data_size>::format<fmt::format_context>(
    seastar::data_size,
    fmt::format_context& ctx) const
    -> decltype(ctx.out());
template
auto fmt::formatter<seastar::data_size>::format<fmt::basic_format_context<std::back_insert_iterator<std::string>, char>>(
    seastar::data_size,
    fmt::basic_format_context<std::back_insert_iterator<std::string>, char>& ctx) const
    -> decltype(ctx.out());
