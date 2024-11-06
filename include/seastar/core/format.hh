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

#include <seastar/core/sstring.hh>
#ifndef SEASTAR_MODULE
#include <fmt/format.h>
#endif

namespace seastar {

/**
 * Evaluate the formatted string in a native fmt library format
 *
 * @param fmt format string with the native fmt library syntax
 * @param a positional parameters
 *
 * @return sstring object with the result of applying the given positional
 *         parameters on a given format string.
 */
template <typename... A>
sstring
format(fmt::format_string<A...> fmt, A&&... a) {
    fmt::memory_buffer out;
    fmt::format_to(fmt::appender(out), fmt, std::forward<A>(a)...);
    return sstring{out.data(), out.size()};
}

}
