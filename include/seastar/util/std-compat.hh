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



#if __has_include(<memory_resource>)
#include <memory_resource>
#else
#include <experimental/memory_resource>
namespace std::pmr {
    using namespace std::experimental::pmr;
}
#endif

#include <source_location>

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

namespace seastar::compat {

// Deprecated: use std::source_location directly.
// This alias is maintained for backwards compatibility with external users.
using source_location
    [[deprecated("Use std::source_location instead of seastar::compat::source_location")]]
    = std::source_location;

}

#if defined(__GNUC__) && !defined(__clang__)
// GCC Workaround: Strip source_location to prevent ICE during RTL expansion.
// See: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=114675
#define SEASTAR_COROUTINE_LOC_PARAM
#define SEASTAR_COROUTINE_LOC_STORE(promise) (void)0
#else
// Standard/Clang: Capture source location naturally.
// Includes the leading comma to mix cleanly into argument lists.
#define SEASTAR_COROUTINE_LOC_PARAM \
    , std::source_location sl = std::source_location::current()

#define SEASTAR_COROUTINE_LOC_STORE(promise) \
    (promise).update_resume_point(sl)
#endif
