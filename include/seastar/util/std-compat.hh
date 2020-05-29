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
 * Copyright (C) 2018 ScyllaDB
 */

#pragma once

#include <optional>
#include <string_view>
#include <variant>

#include <filesystem>

#include <memory_resource>


// Defining SEASTAR_ASAN_ENABLED in here is a bit of a hack, but
// convenient since it is build system independent and in practice
// everything includes this header.

#ifndef __has_feature
#define __has_feature(x) 0
#endif

// clang uses __has_feature, gcc defines __SANITIZE_ADDRESS__
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#define SEASTAR_ASAN_ENABLED
#endif

namespace seastar {

/// \cond internal

namespace compat {

template <typename CharT, typename Traits = std::char_traits<CharT>>
using basic_string_view = std::basic_string_view<CharT, Traits>;

template <typename CharT, typename Traits = std::char_traits<CharT>>
std::string string_view_to_string(const basic_string_view<CharT, Traits>& v) {
    return std::string(v);
}

using string_view = basic_string_view<char>;

} // namespace compat

/// \endcond

} // namespace seastar
