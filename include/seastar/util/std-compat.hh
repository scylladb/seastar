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

#if __has_include(<memory_resource>)
#include <memory_resource>
#else
#include <experimental/memory_resource>
namespace std::pmr {
    using namespace std::experimental::pmr;
}
#endif

#if defined(__cpp_impl_coroutine) || defined(__cpp_coroutines)
#define SEASTAR_COROUTINES_ENABLED
#endif

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

#if __has_include(<source_location>)
#include <source_location>
#endif

#if defined(__cpp_lib_source_location) && !defined(SEASTAR_BROKEN_SOURCE_LOCATION)
namespace seastar::compat {
using source_location = std::source_location;
}
#elif __has_include(<experimental/source_location>) && !defined(SEASTAR_BROKEN_SOURCE_LOCATION)
#include <experimental/source_location>
namespace seastar::compat {
using source_location = std::experimental::source_location;
}
#else
#include <seastar/util/source_location-compat.hh>
namespace seastar::compat {
using source_location = seastar::internal::source_location;
}
#endif
