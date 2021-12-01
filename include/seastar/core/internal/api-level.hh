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
#define SEASTAR_API_LEVEL 3
#endif

#if SEASTAR_API_LEVEL == 6
#define SEASTAR_INCLUDE_API_V6 inline
#else
#define SEASTAR_INCLUDE_API_V6
#endif

#if SEASTAR_API_LEVEL == 5
#define SEASTAR_INCLUDE_API_V5 inline
#else
#define SEASTAR_INCLUDE_API_V5
#endif

#if SEASTAR_API_LEVEL == 4
#define SEASTAR_INCLUDE_API_V4 inline
#else
#define SEASTAR_INCLUDE_API_V4
#endif

#if SEASTAR_API_LEVEL == 3
#define SEASTAR_INCLUDE_API_V3 inline
#else
#define SEASTAR_INCLUDE_API_V3
#endif

#if SEASTAR_API_LEVEL == 2
#define SEASTAR_INCLUDE_API_V2 inline
#else
#define SEASTAR_INCLUDE_API_V2
#endif

// Declare them here so we don't have to use the macros everywhere
namespace seastar {
    SEASTAR_INCLUDE_API_V2 namespace api_v2 {
    }
    SEASTAR_INCLUDE_API_V3 namespace api_v3 {
        inline namespace and_newer {
        }
    }
    SEASTAR_INCLUDE_API_V4 namespace api_v4 {
        inline namespace and_newer {
            using namespace api_v3::and_newer;
        }
    }
    SEASTAR_INCLUDE_API_V5 namespace api_v5 {
        inline namespace and_newer {
            using namespace api_v4::and_newer;
        }
    }
    SEASTAR_INCLUDE_API_V6 namespace api_v6 {
        inline namespace and_newer {
            using namespace api_v5::and_newer;
        }
    }
}
