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

#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <time.h>
#include <sstream>
#include <seastar/core/loop.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/iostream.hh>

namespace seastar {

namespace json {

class jsonable;

typedef struct tm date_time;

/**
 * The formatter prints json values in a json format
 * it overload to_json method for each of the supported format
 * all to_json parameters are passed as a pointer
 */
class formatter {
    enum class state {
        none, array, map
    };
    static sstring begin(state);
    static sstring end(state);

    template<typename K, typename V>
    static sstring to_json(state s, const std::pair<K, V>& p) {
        return s == state::array ?
                        "{" + to_json(state::none, p) + "}" :
                        to_json(p.first) + ":" + to_json(p.second);
    }

    template<typename Iter>
    static sstring to_json(state s, Iter i, Iter e) {
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

    template<typename K, typename V>
    static future<> write(output_stream<char>& stream, state s, const std::pair<K, V>& p) {
        if (s == state::array) {
            return stream.write("{").then([&stream, &p] {
                return write(stream, state::none, p).then([&stream] {
                   return stream.write("}");
                });
            });
        } else {
            return stream.write(to_json(p.first) + ":").then([&p, &stream] {
                return write(stream, p.second);
            });
        }
    }

    template<typename Iter>
    static future<> write(output_stream<char>& stream, state s, Iter i, Iter e) {
        return do_with(true, [&stream, s, i, e] (bool& first) {
            return stream.write(begin(s)).then([&first, &stream, s, i, e] {
                return do_for_each(i, e, [&first, &stream] (auto& m) {
                    auto f = (first) ? make_ready_future<>() : stream.write(",");
                    first = false;
                    return f.then([&m, &stream] {
                        return write(stream, m);
                    });
                }).then([&stream, s] {
                    return stream.write(end(s));
                });
            });
        });
    }

    // fallback template
    template<typename T>
    static future<> write(output_stream<char>& stream, state, const T& t) {
        return stream.write(to_json(t));
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
     * return a json formatted list of a given vector of params
     * @param vec the vector to format
     * @return the given vector in a json format
     */
    template<typename... Args>
    static sstring to_json(const std::vector<Args...>& vec) {
        return to_json(state::array, vec.begin(), vec.end());
    }

    template<typename... Args>
    static sstring to_json(const std::map<Args...>& map) {
        return to_json(state::map, map.begin(), map.end());
    }

    template<typename... Args>
    static sstring to_json(const std::unordered_map<Args...>& map) {
        return to_json(state::map, map.begin(), map.end());
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
     * return a json formatted list of a given vector of params
     * @param vec the vector to format
     * @return the given vector in a json format
     */
    template<typename... Args>
    static future<> write(output_stream<char>& s, const std::vector<Args...>& vec) {
        return write(s, state::array, vec.begin(), vec.end());
    }

    template<typename... Args>
    static future<> write(output_stream<char>& s, const std::map<Args...>& map) {
        return write(s, state::map, map.begin(), map.end());
    }

    template<typename... Args>
    static future<> write(output_stream<char>& s, const std::unordered_map<Args...>& map) {
        return write(s, state::map, map.begin(), map.end());
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
    static future<> write(output_stream<char>& s, const jsonable& obj) {
      return s.write(to_json(obj));
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
