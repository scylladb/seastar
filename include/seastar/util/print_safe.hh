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
 * Copyright 2016 ScyllaDB
 */

#pragma once

#include <stdio.h>

namespace seastar {

//
// Collection of async-signal safe printing functions.
//

// Outputs string to stderr.
// Async-signal safe.
inline
void print_safe(const char *str, size_t len) noexcept {
    while (len) {
        auto result = write(STDERR_FILENO, str, len);
        if (result > 0) {
            len -= result;
            str += result;
        } else if (result == 0) {
            break;
        } else {
            if (errno == EINTR) {
                // retry
            } else {
                break; // what can we do?
            }
        }
    }
}

// Outputs string to stderr.
// Async-signal safe.
inline
void print_safe(const char *str) noexcept {
    print_safe(str, strlen(str));
}

// Fills a buffer with a zero-padded hexadecimal representation of an integer.
// For example, convert_zero_padded_hex_safe(buf, 4, uint16_t(12)) fills the buffer with "000c".
template<typename Integral>
SEASTAR_CONCEPT( requires std::is_integral_v<Integral> )
void convert_zero_padded_hex_safe(char *buf, size_t bufsz, Integral n) noexcept {
    const char *digits = "0123456789abcdef";
    memset(buf, '0', bufsz);
    unsigned i = bufsz;
    while (n) {
        assert(i > 0);
        buf[--i] = digits[n & 0xf];
        n >>= 4;
    }
}

// Prints zero-padded hexadecimal representation of an integer to stderr.
// For example, print_zero_padded_hex_safe(uint16_t(12)) prints "000c".
// Async-signal safe.
template<typename Integral>
void print_zero_padded_hex_safe(Integral n) noexcept {
    static_assert(std::is_integral<Integral>::value && !std::is_signed<Integral>::value, "Requires unsigned integrals");

    char buf[sizeof(n) * 2];
    convert_zero_padded_hex_safe(buf, sizeof(buf), n);
    print_safe(buf, sizeof(buf));
}

// Fills a buffer with a decimal representation of an integer.
// The argument bufsz is the maximum size of the buffer.
// For example, print_decimal_safe(buf, 16, 12) prints "12".
template<typename Integral>
SEASTAR_CONCEPT( requires std::is_integral_v<Integral> )
size_t convert_decimal_safe(char *buf, size_t bufsz, Integral n) noexcept {
    static_assert(std::is_integral<Integral>::value && !std::is_signed<Integral>::value, "Requires unsigned integrals");

    char tmp[sizeof(n) * 3];
    unsigned i = bufsz;
    do {
        assert(i > 0);
        tmp[--i] = '0' + n % 10;
        n /= 10;
    } while (n);
    memcpy(buf, tmp + i, sizeof(tmp) - i);
    return sizeof(tmp) - i;
}

// Prints decimal representation of an integer to stderr.
// For example, print_decimal_safe(12) prints "12".
// Async-signal safe.
template<typename Integral>
void print_decimal_safe(Integral n) noexcept {
    char buf[sizeof(n) * 3];
    unsigned i = sizeof(buf);
    auto len = convert_decimal_safe(buf, i, n);
    print_safe(buf, len);
}

}
