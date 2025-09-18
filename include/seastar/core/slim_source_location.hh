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
 * Copyright 2025 ScyllaDB
 */

#pragma once

#include <cstdint>

#if defined(__x86_64__) || defined(__i386__)
#define SSL_HAS_RIP
#define SSL_HAS_RIP_X86
#endif

namespace seastar {
    class slim_source_location {
#if defined(__clang__)
    public:
        constexpr std::int32_t line() const noexcept { return _line; }
        constexpr std::int32_t column() const noexcept { return _column; }
        constexpr const char* file_name() const noexcept { return _file; }
#ifdef SSL_HAS_RIP
        constexpr std::uintptr_t next_address() const noexcept { return _next_address; }
#endif

        [[gnu::always_inline]] slim_source_location(const char* file = __builtin_FILE(), std::int32_t line = __builtin_LINE(), std::int32_t column = __builtin_COLUMN())
                : _file(file), _line(line), _column(column) {
#ifdef SSL_HAS_RIP_X86
            std::uintptr_t rip;
            asm("leaq 0(%%rip), %0":"=r"(rip));
            _next_address = rip;
#endif
        }
    
    private:
        const char* _file;
#ifdef SSL_HAS_RIP
        std::uintptr_t _next_address;
#endif
        std::int32_t _line;
        std::int32_t _column;
#else
    public:
        constexpr std::int32_t line() const noexcept { return 0; }
        constexpr std::int32_t column() const noexcept { return 0; }
        constexpr const char* file_name() const noexcept { return ""; }
        constexpr std::uintptr_t next_address() const noexcept { return 0; }
#endif
    };
}

#ifdef SSL_HAS_RIP
#define SSL_HAS_RIP
#endif
#ifdef SSL_HAS_RIP_X86
#define SSL_HAS_RIP_X86
#endif
