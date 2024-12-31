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

#include <seastar/core/format.hh>
#include <seastar/core/sstring.hh>
#include <seastar/util/modules.hh>
#ifndef SEASTAR_MODULE
#include <fmt/ostream.h>
#include <iostream>
#include <iomanip>
#include <chrono>
#endif

SEASTAR_MODULE_EXPORT
inline
std::ostream&
operator<<(std::ostream&& os, const void* ptr) {
    return os << ptr; // selects non-rvalue version
}

namespace seastar {
template <typename Iterator>
[[deprecated("use fmt::join()")]]
std::string
format_separated(Iterator b, Iterator e, const char* sep = ", ") {
    std::string ret;
    if (b == e) {
        return ret;
    }
    ret += *b++;
    while (b != e) {
        ret += sep;
        ret += *b++;
    }
    return ret;
}

template <typename TimePoint>
struct usecfmt_wrapper {
    TimePoint val;
};

template <typename TimePoint>
inline
usecfmt_wrapper<TimePoint>
usecfmt(TimePoint tp) {
    return { tp };
};

template <typename Clock, typename Rep, typename Period>
std::ostream&
operator<<(std::ostream& os, usecfmt_wrapper<std::chrono::time_point<Clock, std::chrono::duration<Rep, Period>>> tp) {
    auto usec = std::chrono::duration_cast<std::chrono::microseconds>(tp.val.time_since_epoch()).count();
    std::ostream tmp(os.rdbuf());
    tmp << std::setw(12) << (usec / 1000000) << "." << std::setw(6) << std::setfill('0') << (usec % 1000000);
    return os;
}

template <typename... A>
void
log(A&&... a) {
    std::cout << usecfmt(std::chrono::high_resolution_clock::now()) << " ";
    print(std::forward<A>(a)...);
}


// temporary, use fmt::print() instead
template <typename... A>
[[deprecated("use std::format() or fmt::print()")]]
std::ostream&
fmt_print(std::ostream& os, const char* format, A&&... a) {
    fmt::print(os, format, std::forward<A>(a)...);
    return os;
}

}
