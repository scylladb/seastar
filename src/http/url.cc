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

// https://url.spec.whatwg.org/#percent-encoded-bytes — a percent-encoded byte is
// '%' followed by two ASCII hex digits; otherwise the '%' is left in the output.

bool is_ascii_hex_digit(char c) noexcept {
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'F') ||
           (c >= 'a' && c <= 'f');
}

unsigned hex_digit_value(char c) noexcept {
    if (c <= '9') {
        return static_cast<unsigned>(c - '0');
    }
    if (c <= 'F') {
        return static_cast<unsigned>(c - 'A' + 10);
    }
    return static_cast<unsigned>(c - 'a' + 10);
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
            if (i + 2 < in.size() && is_ascii_hex_digit(in[i + 1]) && is_ascii_hex_digit(in[i + 2])) {
                buff[pos++] = static_cast<char>(
                    (hex_digit_value(in[i + 1]) << 4) | hex_digit_value(in[i + 2]));
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
