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
 * Copyright 2015 Cloudius Systems
 */

#ifdef SEASTAR_MODULE
module;
#endif

#include <cmath>
#include <algorithm>
#include <iomanip>
#include <ios>
#include <sstream>
#include <string_view>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/json/formatter.hh>
#include <seastar/json/json_elements.hh>
#endif

namespace seastar {

using namespace std;

namespace json {

sstring formatter::begin(state s) {
    switch (s) {
    case state::array: return "[";
    case state::map: return "{";
    default: return {};
    }
}

sstring formatter::end(state s) {
    switch (s) {
    case state::array: return "]";
    case state::map: return "}";
    default: return {};
    }
}

static inline bool is_control_char(char c) {
    return c >= 0 && c <= 0x1F;
}

static bool needs_escaping(const string_view& str) {
    return std::any_of(str.begin(), str.end(), [] (char c) {
        return is_control_char(c) || c == '"' || c == '\\';
    });
}

static sstring string_view_to_json(const string_view& str) {
    if (!needs_escaping(str)) {
        return seastar::format("\"{}\"", str);
    }

    ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0');
    oss.put('"');
    for (char c : str) {
        switch (c) {
        case '"':
            oss.put('\\').put('"');
            break;
        case '\\':
            oss.put('\\').put('\\');
            break;
        case '\b':
            oss.put('\\').put('b');
            break;
        case '\f':
            oss.put('\\').put('f');
            break;
        case '\n':
            oss.put('\\').put('n');
            break;
        case '\r':
            oss.put('\\').put('r');
            break;
        case '\t':
            oss.put('\\').put('t');
            break;
        default:
            if (is_control_char(c)) {
                oss.put('\\').put('u') << std::setw(4) << static_cast<int>(c);
            } else {
                oss.put(c);
            }
            break;
        }
    }
    oss.put('"');
    return oss.str();
}

sstring formatter::to_json(const sstring& str) {
    return string_view_to_json(str);
}

sstring formatter::to_json(const char* str) {
    return string_view_to_json(str);
}

sstring formatter::to_json(const char* str, size_t len) {
    return string_view_to_json(string_view{str, len});
}

sstring formatter::to_json(int n) {
    return to_string(n);
}

sstring formatter::to_json(unsigned n) {
    return to_string(n);
}

sstring formatter::to_json(long n) {
    return to_string(n);
}

sstring formatter::to_json(float f) {
    if (std::isinf(f)) {
        throw out_of_range("Infinite float value is not supported");
    } else if (std::isnan(f)) {
        throw invalid_argument("Invalid float value");
    }
    return to_sstring(f);
}

sstring formatter::to_json(double d) {
    if (std::isinf(d)) {
        throw out_of_range("Infinite double value is not supported");
    } else if (std::isnan(d)) {
        throw invalid_argument("Invalid double value");
    }
    return to_sstring(d);
}

sstring formatter::to_json(bool b) {
    return (b) ? "true" : "false";
}

sstring formatter::to_json(const date_time& d) {
    // use RFC3339/RFC8601 "internet format"
    // which is stipulated as mandatory for swagger
    // dates
    // Note that this assumes dates are in UTC timezone
    static constexpr const char* TIME_FORMAT = "%FT%TZ";

    char buff[50];
    sstring res = "\"";
    strftime(buff, 50, TIME_FORMAT, &d);
    res += buff;
    return res + "\"";
}

sstring formatter::to_json(const jsonable& obj) {
    return obj.to_json();
}

sstring formatter::to_json(unsigned long l) {
    return to_string(l);
}

}

}
