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
 * Copyright 2014 Cloudius Systems
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <stdint.h>
#include <algorithm>
#if __has_include(<compare>)
#include <compare>
#endif
#include <string>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <initializer_list>
#include <istream>
#include <ostream>
#include <functional>
#include <type_traits>
#include <fmt/format.h>
#if FMT_VERSION >= 110000
#include <fmt/ranges.h>
#endif
#endif
#include <seastar/util/assert.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/modules.hh>
#include <seastar/core/temporary_buffer.hh>

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

template <typename char_type, typename Size, Size max_size, bool NulTerminate = true>
class basic_sstring;

#ifdef SEASTAR_SSTRING
// Older std::string used atomic reference counting and had no small-buffer-optimization.
// At some point the new std::string ABI improved -- no reference counting plus the small
// buffer optimization. However, aliasing seastar::sstring to std::string still ends up
// with a small performance degradation. (FIXME?)
using sstring = basic_sstring<char, uint32_t, 15>;
#else
using sstring = std::string;
#endif

SEASTAR_MODULE_EXPORT_END

namespace internal {
[[noreturn]] void throw_bad_alloc();
[[noreturn]] void throw_sstring_overflow();
[[noreturn]] void throw_sstring_out_of_range();
}

SEASTAR_MODULE_EXPORT
template <typename char_type, typename Size, Size max_size, bool NulTerminate>
class basic_sstring {
    static_assert(
            (std::is_same_v<char_type, char>
             || std::is_same_v<char_type, signed char>
             || std::is_same_v<char_type, unsigned char>),
            "basic_sstring only supports single byte char types");
    union contents {
        struct external_type {
            char_type* str;
            Size size;
            int8_t pad;
        } external;
        struct internal_type {
            char_type str[max_size];
            int8_t size;
        } internal;
        static_assert(sizeof(external_type) <= sizeof(internal_type), "max_size too small");
        static_assert(max_size <= 127, "max_size too large");
    } u;
    bool is_internal() const noexcept {
        return u.internal.size >= 0;
    }
    bool is_external() const noexcept {
        return !is_internal();
    }
    const char_type* str() const noexcept {
        return is_internal() ? u.internal.str : u.external.str;
    }
    char_type* str() noexcept {
        return is_internal() ? u.internal.str : u.external.str;
    }

public:
    using value_type = char_type;
    using traits_type = std::char_traits<char_type>;
    using allocator_type = std::allocator<char_type>;
    using reference = char_type&;
    using const_reference = const char_type&;
    using pointer = char_type*;
    using const_pointer = const char_type*;
    using iterator = char_type*;
    using const_iterator = const char_type*;
    // FIXME: add reverse_iterator and friend
    using difference_type = ssize_t;  // std::make_signed_t<Size> can be too small
    using size_type = Size;
    static constexpr size_type  npos = static_cast<size_type>(-1);
    static constexpr unsigned padding() { return unsigned(NulTerminate); }
public:
    struct initialized_later {};

    basic_sstring() noexcept {
        u.internal.size = 0;
        if (NulTerminate) {
            u.internal.str[0] = '\0';
        }
    }
    basic_sstring(const basic_sstring& x) {
        if (x.is_internal()) {
            u.internal = x.u.internal;
        } else {
            u.internal.size = -1;
            u.external.str = reinterpret_cast<char_type*>(std::malloc(x.u.external.size + padding()));
            if (!u.external.str) {
                internal::throw_bad_alloc();
            }
            std::copy(x.u.external.str, x.u.external.str + x.u.external.size + padding(), u.external.str);
            u.external.size = x.u.external.size;
        }
    }
    basic_sstring(basic_sstring&& x) noexcept {
#pragma GCC diagnostic push
        // Is a small-string construction is followed by this move constructor, then the trailing bytes
        // of x.u are not initialized, but copied. gcc complains, but it is both legitimate to copy
        // these bytes, and more efficient than a variable-size copy
#pragma GCC diagnostic ignored "-Wuninitialized"
        u = x.u;
#pragma GCC diagnostic pop
        x.u.internal.size = 0;
        x.u.internal.str[0] = '\0';
    }
    basic_sstring(initialized_later, size_t size) {
        if (size_type(size) != size) {
            internal::throw_sstring_overflow();
        }
        if (size + padding() <= sizeof(u.internal.str)) {
            if (NulTerminate) {
                u.internal.str[size] = '\0';
            }
            u.internal.size = size;
        } else {
            u.internal.size = -1;
            u.external.str = reinterpret_cast<char_type*>(std::malloc(size + padding()));
            if (!u.external.str) {
                internal::throw_bad_alloc();
            }
            u.external.size = size;
            if (NulTerminate) {
                u.external.str[size] = '\0';
            }
        }
    }
    basic_sstring(const char_type* x, size_t size) {
        if (size_type(size) != size) {
            internal::throw_sstring_overflow();
        }
        if (size + padding() <= sizeof(u.internal.str)) {
            std::copy(x, x + size, u.internal.str);
            if (NulTerminate) {
                u.internal.str[size] = '\0';
            }
            u.internal.size = size;
        } else {
            u.internal.size = -1;
            u.external.str = reinterpret_cast<char_type*>(std::malloc(size + padding()));
            if (!u.external.str) {
                internal::throw_bad_alloc();
            }
            u.external.size = size;
            std::copy(x, x + size, u.external.str);
            if (NulTerminate) {
                u.external.str[size] = '\0';
            }
        }
    }

    basic_sstring(size_t size, char_type x) : basic_sstring(initialized_later(), size) {
        memset(begin(), x, size);
    }

    basic_sstring(const char* x) : basic_sstring(reinterpret_cast<const char_type*>(x), std::strlen(x)) {}
    basic_sstring(std::basic_string<char_type>& x) : basic_sstring(x.c_str(), x.size()) {}
    basic_sstring(std::initializer_list<char_type> x) : basic_sstring(x.begin(), x.end() - x.begin()) {}
    basic_sstring(const char_type* b, const char_type* e) : basic_sstring(b, e - b) {}
    basic_sstring(const std::basic_string<char_type>& s)
        : basic_sstring(s.data(), s.size()) {}
    template <typename InputIterator>
    basic_sstring(InputIterator first, InputIterator last)
            : basic_sstring(initialized_later(), std::distance(first, last)) {
        std::copy(first, last, begin());
    }
    explicit basic_sstring(std::basic_string_view<char_type, traits_type> v)
            : basic_sstring(v.data(), v.size()) {
    }
    ~basic_sstring() noexcept {
        if (is_external()) {
            std::free(u.external.str);
        }
    }
    basic_sstring& operator=(const basic_sstring& x) {
        basic_sstring tmp(x);
        swap(tmp);
        return *this;
    }
    basic_sstring& operator=(basic_sstring&& x) noexcept {
        if (this != &x) {
            this->~basic_sstring();
            new (this) basic_sstring(std::move(x));
        }
        return *this;
    }
    operator std::basic_string<char_type>() const {
        return { str(), size() };
    }

    size_t size() const noexcept {
        return is_internal() ? u.internal.size : u.external.size;
    }

    size_t length() const noexcept {
        return size();
    }

    size_t find(char_type t, size_t pos = 0) const noexcept {
        const char_type* it = str() + pos;
        const char_type* end = str() + size();
        while (it < end) {
            if (*it == t) {
                return it - str();
            }
            it++;
        }
        return npos;
    }

    size_t find(const char_type* c_str, size_t pos, size_t len2) const noexcept {
        SEASTAR_ASSERT(c_str != nullptr || len2 == 0);
        if (pos > size()) {
            return npos;
        }

        if (len2 == 0) {
            return pos;
        }

        const char_type* it = str() + pos;
        const char_type* end = str() + size();
        size_t len1 = end - it;
        if (len1 < len2) {
            return npos;
        }

        char_type f2 = *c_str;
        while (true) {
            len1 = end - it;
            if (len1 < len2) {
                return npos;
            }

            // find the first byte of pattern string matching in source string
            it = traits_type::find(it, len1 - len2 + 1, f2);
            if (it == nullptr) {
                return npos;
            }

            if (traits_type::compare(it, c_str, len2) == 0) {
                return it - str();
            }

            ++it;
        }
    }

    constexpr size_t find(const char_type* s, size_t pos = 0) const noexcept {
        return find(s, pos, traits_type::length(s));
    }

    size_t find(const basic_sstring& s, size_t pos = 0) const noexcept {
        return find(s.str(), pos, s.size());
    }

    template<class StringViewLike,
             std::enable_if_t<std::is_convertible_v<StringViewLike,
                                                    std::basic_string_view<char_type, traits_type>>,
                              int> = 0>
    size_t find(const StringViewLike& sv_like, size_type pos = 0) const noexcept {
        std::basic_string_view<char_type, traits_type> sv = sv_like;
        return find(sv.data(), pos, sv.size());
    }

    /**
     * find_last_of find the last occurrence of c in the string.
     * When pos is specified, the search only includes characters
     * at or before position pos.
     *
     */
    size_t find_last_of (char_type c, size_t pos = npos) const noexcept {
        const char_type* str_start = str();
        if (size()) {
            if (pos >= size()) {
                pos = size() - 1;
            }
            const char_type* p = str_start + pos + 1;
            do {
                p--;
                if (*p == c) {
                    return (p - str_start);
                }
            } while (p != str_start);
        }
        return npos;
    }

    /**
     *  Append a C substring.
     *  @param s  The C string to append.
     *  @param n  The number of characters to append.
     *  @return  Reference to this string.
     */
    basic_sstring& append (const char_type* s, size_t n) {
        basic_sstring ret(initialized_later(), size() + n);
        std::copy(begin(), end(), ret.begin());
        std::copy(s, s + n, ret.begin() + size());
        *this = std::move(ret);
        return *this;
    }

    /**
     *  Resize string and use the specified @c op to modify the content and the length
     *  @param n  new size
     *  @param op the function object used for setting the new content of the string
     */
    template <class Operation>
    requires std::is_invocable_r_v<size_t, Operation, char_type*, size_t>
    void resize_and_overwrite(size_t n, Operation op) {
        if (n > size()) {
            *this = basic_sstring(initialized_later(), n);
        }
        size_t r = std::move(op)(data(), n);
        SEASTAR_ASSERT(r <= n);
        resize(r);
    }

    /**
     *  Resize string.
     *  @param n  new size.
     *  @param c  if n greater than current size character to fill newly allocated space with.
     */
    void resize(size_t n, const char_type c  = '\0') {
        if (n > size()) {
            *this += basic_sstring(n - size(), c);
        } else if (n < size()) {
            if (is_internal()) {
                u.internal.size = n;
                if (NulTerminate) {
                    u.internal.str[n] = '\0';
                }
            } else if (n + padding() <= sizeof(u.internal.str)) {
                *this = basic_sstring(u.external.str, n);
            } else {
                u.external.size = n;
                if (NulTerminate) {
                    u.external.str[n] = '\0';
                }
            }
        }
    }

    /**
     *  Replace characters with a value of a C style substring.
     *
     */
    basic_sstring& replace(size_type pos, size_type n1, const char_type* s,
             size_type n2) {
        if (pos > size()) {
            internal::throw_sstring_out_of_range();
        }

        if (n1 > size() - pos) {
            n1 = size() - pos;
        }

        if (n1 == n2) {
            if (n2) {
                std::copy(s, s + n2, begin() + pos);
            }
            return *this;
        }
        basic_sstring ret(initialized_later(), size() + n2 - n1);
        char_type* p= ret.begin();
        std::copy(begin(), begin() + pos, p);
        p += pos;
        if (n2) {
            std::copy(s, s + n2, p);
        }
        p += n2;
        std::copy(begin() + pos + n1, end(), p);
        *this = std::move(ret);
        return *this;
    }

    template <class InputIterator>
    basic_sstring& replace (const_iterator i1, const_iterator i2,
            InputIterator first, InputIterator last) {
        if (i1 < begin() || i1 > end() || i2 < begin()) {
            internal::throw_sstring_out_of_range();
        }
        if (i2 > end()) {
            i2 = end();
        }

        if (i2 - i1 == last - first) {
            //in place replacement
            std::copy(first, last, const_cast<char_type*>(i1));
            return *this;
        }
        basic_sstring ret(initialized_later(), size() + (last - first) - (i2 - i1));
        char_type* p = ret.begin();
        p = std::copy(cbegin(), i1, p);
        p = std::copy(first, last, p);
        std::copy(i2, cend(), p);
        *this = std::move(ret);
        return *this;
    }

    iterator erase(iterator first, iterator last) {
        size_t pos = first - begin();
        replace(pos, last - first, nullptr, 0);
        return begin() + pos;
    }

    /**
     * Inserts additional characters into the string right before
     * the character indicated by p.
     */
    template <class InputIterator>
    void insert(const_iterator p, InputIterator beg, InputIterator end) {
        replace(p, p, beg, end);
    }


    /**
     *  Returns a read/write reference to the data at the first
     *  element of the string.
     *  This function shall not be called on empty strings.
     */
    reference
    front() noexcept {
        SEASTAR_ASSERT(!empty());
        return *str();
    }

    /**
     *  Returns a  read-only (constant) reference to the data at the first
     *  element of the string.
     *  This function shall not be called on empty strings.
     */
    const_reference
    front() const noexcept {
        SEASTAR_ASSERT(!empty());
        return *str();
    }

    /**
     *  Returns a read/write reference to the data at the last
     *  element of the string.
     *  This function shall not be called on empty strings.
     */
    reference
    back() noexcept {
        return operator[](size() - 1);
    }

    /**
     *  Returns a  read-only (constant) reference to the data at the last
     *  element of the string.
     *  This function shall not be called on empty strings.
     */
    const_reference
    back() const noexcept {
        return operator[](size() - 1);
    }

    basic_sstring substr(size_t from, size_t len = npos)  const {
        if (from > size()) {
            internal::throw_sstring_out_of_range();
        }
        if (len > size() - from) {
            len = size() - from;
        }
        if (len == 0) {
            return "";
        }
        return { str() + from , len };
    }

    const char_type& at(size_t pos) const {
        if (pos >= size()) {
            internal::throw_sstring_out_of_range();
        }
        return *(str() + pos);
    }

    char_type& at(size_t pos) {
        if (pos >= size()) {
            internal::throw_sstring_out_of_range();
        }
        return *(str() + pos);
    }

    bool empty() const noexcept {
        return u.internal.size == 0;
    }

    // Deprecated March 2020.
    [[deprecated("Use = {}")]]
    void reset() noexcept {
        if (is_external()) {
            std::free(u.external.str);
        }
        u.internal.size = 0;
        if (NulTerminate) {
            u.internal.str[0] = '\0';
        }
    }
    temporary_buffer<char_type> release() && {
        if (is_external()) {
            auto ptr = u.external.str;
            auto size = u.external.size;
            u.external.str = nullptr;
            u.external.size = 0;
            return temporary_buffer<char_type>(ptr, size, make_free_deleter(ptr));
        } else {
            auto buf = temporary_buffer<char_type>(u.internal.size);
            std::copy(u.internal.str, u.internal.str + u.internal.size, buf.get_write());
            u.internal.size = 0;
            if (NulTerminate) {
                u.internal.str[0] = '\0';
            }
            return buf;
        }
    }
    int compare(std::basic_string_view<char_type, traits_type> x) const noexcept {
        auto n = traits_type::compare(begin(), x.data(), std::min(size(), x.size()));
        if (n != 0) {
            return n;
        }
        if (size() < x.size()) {
            return -1;
        } else if (size() > x.size()) {
            return 1;
        } else {
            return 0;
        }
    }

    int compare(size_t pos, size_t sz, std::basic_string_view<char_type, traits_type> x) const {
        if (pos > size()) {
            internal::throw_sstring_out_of_range();
        }

        sz = std::min(size() - pos, sz);
        auto n = traits_type::compare(begin() + pos, x.data(), std::min(sz, x.size()));
        if (n != 0) {
            return n;
        }
        if (sz < x.size()) {
            return -1;
        } else if (sz > x.size()) {
            return 1;
        } else {
            return 0;
        }
    }

    constexpr bool starts_with(std::basic_string_view<char_type, traits_type> sv) const noexcept {
        return size() >= sv.size() && compare(0, sv.size(), sv) == 0;
    }

    constexpr bool starts_with(char_type c) const noexcept {
        return !empty() && traits_type::eq(front(), c);
    }

    constexpr bool starts_with(const char_type* s) const noexcept {
        return starts_with(std::basic_string_view<char_type, traits_type>(s));
    }

    constexpr bool ends_with(std::basic_string_view<char_type, traits_type> sv) const noexcept {
        return size() >= sv.size() && compare(size() - sv.size(), npos, sv) == 0;
    }

    constexpr bool ends_with(char_type c) const noexcept {
        return !empty() && traits_type::eq(back(), c);
    }

    constexpr bool ends_with(const char_type* s) const noexcept {
        return ends_with(std::basic_string_view<char_type, traits_type>(s));
    }

    constexpr bool contains(std::basic_string_view<char_type, traits_type> sv) const noexcept {
        return find(sv) != npos;
    }

    constexpr bool contains(char_type c) const noexcept {
        return find(c) != npos;
    }

    constexpr bool contains(const char_type* s) const noexcept {
        return find(s) != npos;
    }

    void swap(basic_sstring& x) noexcept {
        contents tmp;
        tmp = x.u;
        x.u = u;
        u = tmp;
    }
    char_type* data() noexcept {
        return str();
    }
    const char_type* data() const noexcept {
        return str();
    }
    const char_type* c_str() const noexcept {
        return str();
    }
    const char_type* begin() const noexcept { return str(); }
    const char_type* end() const noexcept { return str() + size(); }
    const char_type* cbegin() const noexcept { return str(); }
    const char_type* cend() const noexcept { return str() + size(); }
    char_type* begin() noexcept { return str(); }
    char_type* end() noexcept { return str() + size(); }
    bool operator==(const basic_sstring& x) const noexcept {
        return size() == x.size() && std::equal(begin(), end(), x.begin());
    }
    bool operator!=(const basic_sstring& x) const noexcept {
        return !operator==(x);
    }
    constexpr bool operator==(const std::basic_string<char_type> x) const noexcept {
        return compare(x) == 0;
    }
    constexpr bool operator==(const char_type* x) const noexcept {
        return compare(x) == 0;
    }
#if __cpp_lib_three_way_comparison
    constexpr std::strong_ordering operator<=>(const auto& x) const noexcept {
        return compare(x) <=> 0;
    }
#else
    bool operator<(const basic_sstring& x) const noexcept {
        return compare(x) < 0;
    }
#endif
    basic_sstring operator+(const basic_sstring& x) const {
        basic_sstring ret(initialized_later(), size() + x.size());
        std::copy(begin(), end(), ret.begin());
        std::copy(x.begin(), x.end(), ret.begin() + size());
        return ret;
    }
    basic_sstring& operator+=(const basic_sstring& x) {
        return *this = *this + x;
    }
    char_type& operator[](size_type pos) noexcept {
        return str()[pos];
    }
    const char_type& operator[](size_type pos) const noexcept {
        return str()[pos];
    }

    operator std::basic_string_view<char_type>() const noexcept {
        // we assume that std::basic_string_view<char_type>(str(), size())
        // won't throw, although it is not specified as noexcept in
        // https://en.cppreference.com/w/cpp/string/basic_string_view/basic_string_view
        // at this time (C++20).
        //
        // This is similar to std::string operator std::basic_string_view:
        // https://en.cppreference.com/w/cpp/string/basic_string/operator_basic_string_view
        // that is specified as noexcept too.
        static_assert(noexcept(std::basic_string_view<char_type>(str(), size())));
        return std::basic_string_view<char_type>(str(), size());
    }
};

namespace internal {
template <class T> struct is_sstring : std::false_type {};
template <typename char_type, typename Size, Size max_size, bool NulTerminate>
struct is_sstring<basic_sstring<char_type, Size, max_size, NulTerminate>> : std::true_type {};
}

SEASTAR_MODULE_EXPORT
template <typename string_type = sstring>
string_type uninitialized_string(size_t size) {
    if constexpr (internal::is_sstring<string_type>::value) {
        return string_type(typename string_type::initialized_later(), size);
    } else {
        string_type ret;
#ifdef __cpp_lib_string_resize_and_overwrite
        ret.resize_and_overwrite(size, [](typename string_type::value_type*, typename string_type::size_type n) { return n; });
#else
        ret.resize(size);
#endif
        return ret;
    }
}

SEASTAR_MODULE_EXPORT
template <typename char_type, typename size_type, size_type Max, size_type N, bool NulTerminate>
inline
basic_sstring<char_type, size_type, Max, NulTerminate>
operator+(const char(&s)[N], const basic_sstring<char_type, size_type, Max, NulTerminate>& t) {
    using sstring = basic_sstring<char_type, size_type, Max, NulTerminate>;
    // don't copy the terminating NUL character
    sstring ret(typename sstring::initialized_later(), N-1 + t.size());
    auto p = std::copy(std::begin(s), std::end(s)-1, ret.begin());
    std::copy(t.begin(), t.end(), p);
    return ret;
}

template <typename T>
static inline
size_t constexpr str_len(const T& s) {
    return std::string_view(s).size();
}

SEASTAR_MODULE_EXPORT
template <typename char_type, typename size_type, size_type max_size>
inline
void swap(basic_sstring<char_type, size_type, max_size>& x,
          basic_sstring<char_type, size_type, max_size>& y) noexcept
{
    return x.swap(y);
}

SEASTAR_MODULE_EXPORT
template <typename char_type, typename size_type, size_type max_size, bool NulTerminate, typename char_traits>
inline
std::basic_ostream<char_type, char_traits>&
operator<<(std::basic_ostream<char_type, char_traits>& os,
        const basic_sstring<char_type, size_type, max_size, NulTerminate>& s) {
    return os.write(s.begin(), s.size());
}

SEASTAR_MODULE_EXPORT
template <typename char_type, typename size_type, size_type max_size, bool NulTerminate, typename char_traits>
inline
std::basic_istream<char_type, char_traits>&
operator>>(std::basic_istream<char_type, char_traits>& is,
        basic_sstring<char_type, size_type, max_size, NulTerminate>& s) {
    std::string tmp;
    is >> tmp;
    s = tmp;
    return is;
}

}

namespace std {

SEASTAR_MODULE_EXPORT
template <typename char_type, typename size_type, size_type max_size, bool NulTerminate>
struct hash<seastar::basic_sstring<char_type, size_type, max_size, NulTerminate>> {
    size_t operator()(const seastar::basic_sstring<char_type, size_type, max_size, NulTerminate>& s) const {
        return std::hash<std::basic_string_view<char_type>>()(s);
    }
};

}

namespace seastar {

template <typename T>
static inline
void copy_str_to(char*& dst, const T& s) {
    std::string_view v(s);
    dst = std::copy(v.begin(), v.end(), dst);
}

template <typename String = sstring, typename... Args>
static String make_sstring(Args&&... args)
{
    String ret = uninitialized_string<String>((str_len(args) + ...));
    auto dst = ret.data();
    (copy_str_to(dst, args), ...);
    return ret;
}

namespace internal {
template <typename string_type, typename T>
string_type to_sstring(T value) {
    auto size = fmt::formatted_size("{}", value);
    auto formatted = uninitialized_string<string_type>(size);
    fmt::format_to(formatted.data(), "{}", value);
    return formatted;
}

template <typename string_type>
string_type to_sstring(const char* value) {
    return string_type(value);
}

template <typename string_type>
string_type to_sstring(sstring value) {
    return value;
}

template <typename string_type>
string_type to_sstring(const temporary_buffer<char>& buf) {
    return string_type(buf.get(), buf.size());
}
}

template <typename string_type = sstring, typename T>
string_type to_sstring(T value) {
    return internal::to_sstring<string_type>(value);
}
}

#ifdef SEASTAR_DEPRECATED_OSTREAM_FORMATTERS

namespace std {

SEASTAR_MODULE_EXPORT
template <typename T>
[[deprecated("Use {fmt} instead")]]
inline
std::ostream& operator<<(std::ostream& os, const std::vector<T>& v) {
    bool first = true;
    os << "{";
    for (auto&& elem : v) {
        if (!first) {
            os << ", ";
        } else {
            first = false;
        }
        os << elem;
    }
    os << "}";
    return os;
}

SEASTAR_MODULE_EXPORT
template <typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator>
[[deprecated("Use {fmt} instead")]]
std::ostream& operator<<(std::ostream& os, const std::unordered_map<Key, T, Hash, KeyEqual, Allocator>& v) {
    bool first = true;
    os << "{";
    for (auto&& elem : v) {
        if (!first) {
            os << ", ";
        } else {
            first = false;
        }
        os << "{" << elem.first << " -> " << elem.second << "}";
    }
    os << "}";
    return os;
}
}

#endif

#if FMT_VERSION >= 110000

template <typename char_type, typename Size, Size max_size, bool NulTerminate>
struct fmt::range_format_kind<seastar::basic_sstring<char_type, Size, max_size, NulTerminate>, char_type> : std::integral_constant<fmt::range_format, fmt::range_format::disabled>
{};

#endif

#if FMT_VERSION >= 90000

// Due to https://github.com/llvm/llvm-project/issues/68849, we inherit
// from formatter<string_view> publicly rather than privately

SEASTAR_MODULE_EXPORT
template <typename char_type, typename Size, Size max_size, bool NulTerminate>
struct fmt::formatter<seastar::basic_sstring<char_type, Size, max_size, NulTerminate>>
    : public fmt::formatter<basic_string_view<char_type>> {
    using format_as_t = basic_string_view<char_type>;
    using base = fmt::formatter<format_as_t>;
    template <typename FormatContext>
    auto format(const ::seastar::basic_sstring<char_type, Size, max_size, NulTerminate>& s, FormatContext& ctx) const {
        return base::format(format_as_t{s.c_str(), s.size()}, ctx);
    }
};

#endif
