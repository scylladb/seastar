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
 * Copyright (C) 2025 Redpanda Data.
 */

#pragma once

namespace seastar::internal {
[[noreturn]] void assert_fail(const char *msg, const char *file, int line,
                              const char *func);
}

/// Like assert(), but independent of NDEBUG. Active in all build modes.
#define SEASTAR_ASSERT(x)                                             \
    do {                                                              \
        if (!(x)) [[unlikely]] {                                      \
            seastar::internal::assert_fail(#x, __FILE__, __LINE__,    \
                                           __PRETTY_FUNCTION__);      \
        }                                                             \
    } while (0)
