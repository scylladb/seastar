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
 * Copyright (C) 2022 Scylladb, Ltd.
 */


#include <string_view>

#include <seastar/http/url.hh>

namespace seastar {
namespace http {
namespace internal {

namespace {

bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

short hex_to_byte(char c) {
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    } else if (c >= '0' && c <= '9') {
        return c - '0';
    }
    return -1;
}

/**
 * Convert a hex encoded 2 bytes substring to char.
 * Returns -1 if either character is not a valid hex digit.
 */
short hexstr_to_char(std::string_view in, size_t from) {
    auto hi = hex_to_byte(in[from]);
    auto lo = hex_to_byte(in[from + 1]);
    if (hi < 0 || lo < 0) {
        return -1;
    }
    return static_cast<short>(hi * 16 + lo);
}

bool should_encode(char c) {
    return !(
        (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        (c == '-' || c == '_' || c == '.' || c == '~')
    );
}

inline char char_to_hex(unsigned char val) {
    return "0123456789ABCDEF"[val];
}

bool decode(bool replace_plus, std::string_view in, sstring& out) {
    size_t pos = 0;
    sstring buff(in.length(), 0);
    for (size_t i = 0; i < in.length(); ++i) {
        if (in[i] == '%') {
            if (i + 2 < in.size() && is_hex_digit(in[i + 1]) && is_hex_digit(in[i + 2])) {
                buff[pos++] = static_cast<char>(hexstr_to_char(in, i + 1));
                i += 2;
            } else {
                buff[pos++] = '%';
            }
        } else if (replace_plus && in[i] == '+') {
            buff[pos++] = ' ';
        } else {
            buff[pos++] = in[i];
        }
    }
    buff.resize(pos);
    out = std::move(buff);
    return true;
}

}

bool url_decode(std::string_view in, sstring& out) {
    return decode(true, in, out);
}

bool path_decode(std::string_view in, sstring& out) {
    return decode(false, in, out);
}


sstring url_encode(std::string_view in) {
    size_t encodable_chars = 0;
    for (size_t i = 0; i < in.length(); i++) {
        if (should_encode(in[i])) {
            encodable_chars++;
        }
    }

    if (encodable_chars == 0) {
        return sstring(in);
    }

    sstring ret(in.length() + encodable_chars * 2, 0);
    size_t o = 0;
    for (size_t i = 0; i < in.length(); i++) {
        if (should_encode(in[i])) {
            ret[o++] = '%';
            ret[o++] = char_to_hex(((unsigned char)in[i]) >> 4);
            ret[o++] = char_to_hex(in[i] & 0xF);
        } else {
            ret[o++] = in[i];
        }
    }
    return ret;
}

} // internal namespace
} // http namespace
} // seastar namespace
