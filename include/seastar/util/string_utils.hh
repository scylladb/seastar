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
 * Copyright 2015 Cloudius Systems
 */

//
// request.hpp
// ~~~~~~~~~~~
//
// Copyright (c) 2003-2013 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <cstring>
#include <stdio.h>

#include <seastar/core/sstring.hh>

namespace seastar {

namespace internal {

//
// Collection of utilities for working with strings .
//

struct string_view_hash {
    using is_transparent = void;
    size_t operator()(const char *txt) const {
        return std::hash<std::string_view>{}(txt);
    }
    size_t operator()(std::string_view txt) const {
        return std::hash<std::string_view>{}(txt);
    }
    size_t operator()(const sstring &txt) const {
        return std::hash<sstring>()(txt);
    }
};

struct case_insensitive_cmp {
    bool operator()(const sstring& s1, const sstring& s2) const {
        return std::equal(s1.begin(), s1.end(), s2.begin(), s2.end(),
                [](char a, char b) { return ::tolower(a) == ::tolower(b); });
    }
};

struct case_insensitive_hash {
    size_t operator()(sstring s) const {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return std::hash<sstring>()(s);
    }
};


}

}
