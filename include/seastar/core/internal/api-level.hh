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
 * Copyright (C) 2019 ScyllaDB
 */

#pragma once

// For IDEs that don't see SEASTAR_API_LEVEL, generate a nice default
#ifndef SEASTAR_API_LEVEL
#define SEASTAR_API_LEVEL 9
#endif

#if SEASTAR_API_LEVEL == 8
#define SEASTAR_INCLUDE_API_V8 inline
#else
#define SEASTAR_INCLUDE_API_V8
#endif

// Declare them here so we don't have to use the macros everywhere
namespace seastar {
    SEASTAR_INCLUDE_API_V8 namespace api_v8 {
        inline namespace and_newer {
        }
    }
}

// Helpers for ignoring deprecation warnings in code that has to deal with
// deprecated APIs, e.g., constructors/etc for structs with deprecated fields.
#define SEASTAR_INTERNAL_BEGIN_IGNORE_DEPRECATIONS \
    _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wdeprecated-declarations\"")

#define SEASTAR_INTERNAL_END_IGNORE_DEPRECATIONS \
    _Pragma("GCC diagnostic pop")
