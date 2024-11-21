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

#pragma once

#ifndef SEASTAR_MODULE
#include <ranges>
#include <string>
#include <tuple>
#include <vector>
#include <unordered_map>
#include <map>
#include <time.h>
#include <sstream>
#endif

#include <seastar/core/loop.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/iostream.hh>
#include <seastar/util/modules.hh>

namespace seastar {

namespace internal {

template<typename T>
concept is_map = requires {
    typename T::mapped_type;
};

template<typename T>
concept is_pair_like = requires {
    typename std::tuple_size<T>::type;
    requires std::tuple_size_v<T> == 2;
};

template<typename T>
concept is_string_like =
    std::convertible_to<const T&, std::string_view> &&
    requires (T s) {
        { s.data() } -> std::same_as<char*>;
        // sstring::length() and sstring::find() return size_t instead of
        // size_type (i.e., uint32_t), so we cannot check their return type
        // with T::size_type
        s.find('a');
        s.length();
    };

}

namespace json {

SEASTAR_MODULE_EXPORT
class jsonable;

typedef struct tm date_time;

/**
 * The formatter prints json values in a json format
 * it overload to_json method for each of the supported format
 * all to_json parameters are passed as a pointer
 */
SEASTAR_MODULE_EXPORT
class formatter {
    enum class state {
        none, array, map
    };
    static sstring begin(state);
    static sstring end(state);

    template<internal::is_pair_like T>
    static sstring to_json(state s, const T& p) {
        auto& [key, value] = p;
        return s == state::array ?
                        "{" + to_json(state::none, p) + "}" :
                        to_json(key) + ":" + to_json(value);
    }

    template<typename Iterator, typename Sentinel>
    static sstring to_json(state s, Iterator i, Sentinel e) {
        std::stringstream res;
        res << begin(s);
        size_t n = 0;
        while (i != e) {
            if (n++ != 0) {
                res << ",";
            }
            res << to_json(s, *i++);
        }
        res << end(s);
        return res.str();
    }

    // fallback template
    template<typename T>
    static sstring to_json(state, const T& t) {
        return to_json(t);
    }

    template<internal::is_pair_like T>
    static future<> write(output_stream<char>& stream, state s, T&& p) {
        if (s == state::array) {
            return stream.write("{").then([&stream, &p] {
                return write(stream, state::none, p).then([&stream] {
                   return stream.write("}");
                });
            });
        } else {
            auto& [key, value] = p;
            return stream.write(to_json(key) + ":").then([&value, &stream] {
                return write(stream, value);
            });
        }
    }

    template<internal::is_pair_like T>
    static future<> write(output_stream<char>& stream, state s, const T& p) {
        if (s == state::array) {
            return stream.write("{").then([&stream, p] {
                return write(stream, state::none, p).then([&stream] {
                   return stream.write("}");
                });
            });
        } else {
            auto& [key, value] = p;
            return stream.write(to_json(key) + ":").then([&stream, value] {
                return write(stream, value);
            });
        }
    }

    template<typename Iterator, typename Sentinel>
    static future<> write(output_stream<char>& stream, state s, Iterator i, Sentinel e) {
        return do_with(true, [&stream, s, i, e] (bool& first) {
            return stream.write(begin(s)).then([&first, &stream, s, i, e] {
                using ref_t = std::iter_reference_t<Iterator>;
                return do_for_each(i, e, [&first, &stream, s] (ref_t m) {
                    auto f = (first) ? make_ready_future<>() : stream.write(",");
                    first = false;
                    if constexpr (std::is_lvalue_reference_v<ref_t>) {
                       return f.then([&m, &stream, s] {
                           return write(stream, s, m);
                       });
                    } else {
                        using value_t = std::iter_value_t<Iterator>;
                        return f.then([m = std::forward<value_t>(m), &stream, s] {
                            return write(stream, s, m);
                        });
                    }
                }).then([&stream, s] {
                    return stream.write(end(s));
                });
            });
        });
    }

    // fallback template
    template<typename T>
    static future<> write(output_stream<char>& stream, state, const T& t) {
        return write(stream, t);
    }

public:

    /**
     * return a json formatted string
     * @param str the string to format
     * @return the given string in a json format
     */
    static sstring to_json(const sstring& str);

    /**
     * return a json formatted int
     * @param n the int to format
     * @return the given int in a json format
     */
    static sstring to_json(int n);

    /**
     * return a json formatted unsigned
     * @param n the unsigned to format
     * @return the given unsigned in a json format
     */
    static sstring to_json(unsigned n);

    /**
     * return a json formatted long
     * @param n the long to format
     * @return the given long in a json format
     */
    static sstring to_json(long n);

    /**
     * return a json formatted float
     * @param f the float to format
     * @return the given float in a json format
     */
    static sstring to_json(float f);

    /**
     * return a json formatted double
     * @param d the double to format
     * @return the given double in a json format
     */
    static sstring to_json(double d);

    /**
     * return a json formatted char* (treated as string), possibly with zero-chars in the middle
     * @param str the char* to format
     * @param len number of bytes to read from the \p str
     * @return the given char* in a json format
     */
    static sstring to_json(const char* str, size_t len);

    /**
     * return a json formatted char* (treated as string), assuming there are no zero-chars in the middle
     * @param str the char* to format
     * @return the given char* in a json format
     * @deprecated A more robust overload should be preferred: \ref to_json(const char*, size_t)
     */
    static sstring to_json(const char* str);

    /**
     * return a json formatted bool
     * @param d the bool to format
     * @return the given bool in a json format
     */
    static sstring to_json(bool d);

    /**
     * converts a given range to a JSON-formatted string
     * @param range A standard range type
     * @return A string containing the JSON representation of the input range
     */
    template<std::ranges::input_range Range>
    requires (!internal::is_string_like<Range>)
    static sstring to_json(const Range& range) {
        if constexpr (internal::is_map<Range>) {
            return to_json(state::map, std::ranges::begin(range), std::ranges::end(range));
        } else {
            return to_json(state::array, std::ranges::begin(range), std::ranges::end(range));
        }
    }

    /**
     * return a json formatted date_time
     * @param d the date_time to format
     * @return the given date_time in a json format
     */
    static sstring to_json(const date_time& d);

    /**
     * return a json formatted json object
     * @param obj the date_time to format
     * @return the given json object in a json format
     */
    static sstring to_json(const jsonable& obj);

    /**
     * return a json formatted unsigned long
     * @param l unsigned long to format
     * @return the given unsigned long in a json format
     */
    static sstring to_json(unsigned long l);



    /**
     * return a json formatted string
     * @param str the string to format
     * @return the given string in a json format
     */
    static future<> write(output_stream<char>& s, const sstring& str) {
        return s.write(to_json(str));
    }

    /**
     * return a json formatted int
     * @param n the int to format
     * @return the given int in a json format
     */
    static future<> write(output_stream<char>& s, int n) {
        return s.write(to_json(n));
    }

    /**
     * return a json formatted long
     * @param n the long to format
     * @return the given long in a json format
     */
    static future<> write(output_stream<char>& s, long n) {
        return s.write(to_json(n));
    }

    /**
     * return a json formatted float
     * @param f the float to format
     * @return the given float in a json format
     */
    static future<> write(output_stream<char>& s, float f) {
        return s.write(to_json(f));
    }

    /**
     * return a json formatted double
     * @param d the double to format
     * @return the given double in a json format
     */
    static future<> write(output_stream<char>& s, double d) {
        return s.write(to_json(d));
    }

    /**
     * return a json formatted char* (treated as string)
     * @param str the char* to format
     * @return the given char* in a json format
     */
    static future<> write(output_stream<char>& s, const char* str) {
        return s.write(to_json(str));
    }

    /**
     * return a json formatted bool
     * @param d the bool to format
     * @return the given bool in a json format
     */
    static future<> write(output_stream<char>& s, bool d) {
        return s.write(to_json(d));
    }

    /**
     * Converts a range to a JSON array or object and writes it to an output stream.
     * @param s     The output stream that will receive the JSON-formatted string
     * @param range The range to convert. If the range contains key-value pairs (like std::map),
     *              it will be formatted as a JSON object. Otherwise, it will be formatted as
     *              a JSON array.
     * @returns     A future that will be resolved when the write operation completes
     *
     */
    template<std::ranges::input_range Range>
    requires (!internal::is_string_like<Range>)
    static future<> write(output_stream<char>& s, const Range& range) {
        return do_with(std::move(range), [&s] (const auto& range) {
            if constexpr (internal::is_map<Range>) {
                return write(s, state::map, std::ranges::begin(range), std::ranges::end(range));
            } else {
                return write(s, state::array, std::ranges::begin(range), std::ranges::end(range));
            }
        });
    }

    /**
     * return a json formatted date_time
     * @param d the date_time to format
     * @return the given date_time in a json format
     */
    static future<> write(output_stream<char>& s, const date_time& d) {
      return s.write(to_json(d));
     }

    /**
     * return a json formatted json object
     * @param obj the date_time to format
     * @return the given json object in a json format
     */
    template <std::derived_from<jsonable> Jsonable>
    static future<> write(output_stream<char>& s, Jsonable obj) {
        return do_with(std::move(obj), [&s] (const auto& obj) {
            return obj.write(s);
        });
    }

    /**
     * return a json formatted unsigned long
     * @param l unsigned long to format
     * @return the given unsigned long in a json format
     */
    static future<> write(output_stream<char>& s, unsigned long l) {
      return s.write(to_json(l));
     }
};

}

}
