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

#ifndef CONVERSIONS_CC_
#define CONVERSIONS_CC_

#include <seastar/util/conversions.hh>
#include <seastar/core/print.hh>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <cctype>

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

#endif /* CONVERSIONS_CC_ */
