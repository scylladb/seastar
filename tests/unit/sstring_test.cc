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

#define BOOST_TEST_MODULE core

#include <boost/test/unit_test.hpp>
#include <seastar/core/sstring.hh>
#include <list>

using namespace seastar;

BOOST_AUTO_TEST_CASE(test_make_sstring) {
    std::string_view foo = "foo";
    std::string bar = "bar";
    sstring zed = "zed";
    const char* baz = "baz";
    BOOST_REQUIRE_EQUAL(make_sstring(foo, bar, zed, baz, "bah"), sstring("foobarzedbazbah"));
}

BOOST_AUTO_TEST_CASE(test_construction) {
    BOOST_REQUIRE_EQUAL(sstring(std::string_view("abc")), sstring("abc"));
}

BOOST_AUTO_TEST_CASE(test_equality) {
    BOOST_REQUIRE_EQUAL(sstring("aaa"), sstring("aaa"));
}

BOOST_AUTO_TEST_CASE(test_to_sstring) {
    BOOST_REQUIRE_EQUAL(to_sstring(1234567), sstring("1234567"));
}

BOOST_AUTO_TEST_CASE(test_add_literal_to_sstring) {
    BOOST_REQUIRE_EQUAL("x" + sstring("y"), sstring("xy"));
}

BOOST_AUTO_TEST_CASE(test_find_sstring) {
    BOOST_REQUIRE_EQUAL(sstring("abcde").find('b'), 1u);
    BOOST_REQUIRE_EQUAL(sstring("babcde").find('b',1), 2u);
}

BOOST_AUTO_TEST_CASE(test_find_sstring_compatible) {
    auto check_find = [](const char* s1, const char* s2, size_t pos) {
        const auto xpos_ss = sstring(s1).find(s2, pos);
        const auto xpos_std = std::string(s1).find(s2, pos);

        // verify that std::string really has the same behavior as we just tested for sstring
        if (xpos_ss == sstring::npos) {  // sstring::npos may not equal std::string::npos ?
            BOOST_REQUIRE_EQUAL(xpos_std, std::string::npos);
        } else {
            BOOST_REQUIRE_EQUAL(xpos_ss, xpos_std);
        }
    };

    check_find("", "", 0);
    check_find("", "", 1);
    check_find("abcde", "", 0);
    check_find("abcde", "", 1);
    check_find("abcde", "", 5);
    check_find("abcde", "", 6);
}

BOOST_AUTO_TEST_CASE(test_not_find_sstring) {
    BOOST_REQUIRE_EQUAL(sstring("abcde").find('x'), sstring::npos);
}

BOOST_AUTO_TEST_CASE(test_str_find_sstring) {
    BOOST_REQUIRE_EQUAL(sstring("abcde").find("bc"), 1u);
    BOOST_REQUIRE_EQUAL(sstring("abcbcde").find("bc", 2), 3u);
    BOOST_REQUIRE_EQUAL(sstring("abcde").find("abcde"), 0u);
    BOOST_REQUIRE_EQUAL(sstring("abcde").find("", 5), 5u);
    BOOST_REQUIRE_EQUAL(sstring("ababcbdbef").find("bef"), 7u);
    BOOST_REQUIRE_EQUAL(sstring("").find("", 0), 0u);
}

BOOST_AUTO_TEST_CASE(test_str_not_find_sstring) {
    BOOST_REQUIRE_EQUAL(sstring("abcde").find("x"), sstring::npos);
    BOOST_REQUIRE_EQUAL(sstring("abcdefg").find("cde", 6), sstring::npos);
    BOOST_REQUIRE_EQUAL(sstring("abcdefg").find("cde", 4), sstring::npos);
    BOOST_REQUIRE_EQUAL(sstring("ababcbdbe").find("bcd"), sstring::npos);
    BOOST_REQUIRE_EQUAL(sstring("").find("", 1), sstring::npos);
    BOOST_REQUIRE_EQUAL(sstring("abc").find("abcde"), sstring::npos);
}

BOOST_AUTO_TEST_CASE(test_substr_sstring) {
    BOOST_REQUIRE_EQUAL(sstring("abcde").substr(1,2), "bc");
    BOOST_REQUIRE_EQUAL(sstring("abc").substr(1,2), "bc");
    BOOST_REQUIRE_EQUAL(sstring("abc").substr(1,3), "bc");
    BOOST_REQUIRE_EQUAL(sstring("abc").substr(0, 2), "ab");
    BOOST_REQUIRE_EQUAL(sstring("abc").substr(3, 2), "");
    BOOST_REQUIRE_EQUAL(sstring("abc").substr(1), "bc");
}

BOOST_AUTO_TEST_CASE(test_substr_eor_sstring) {
    BOOST_REQUIRE_THROW(sstring("abcde").substr(6,1), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(test_at_sstring) {
    BOOST_REQUIRE_EQUAL(sstring("abcde").at(1), 'b');
    BOOST_REQUIRE_THROW(sstring("abcde").at(6), std::out_of_range);
    sstring s("abcde");
    s.at(1) = 'd';
    BOOST_REQUIRE_EQUAL(s, "adcde");
}

BOOST_AUTO_TEST_CASE(test_find_last_sstring) {
    BOOST_REQUIRE_EQUAL(sstring("ababa").find_last_of('a'), 4u);
    BOOST_REQUIRE_EQUAL(sstring("ababa").find_last_of('a',5), 4u);
    BOOST_REQUIRE_EQUAL(sstring("ababa").find_last_of('a',4), 4u);
    BOOST_REQUIRE_EQUAL(sstring("ababa").find_last_of('a',3), 2u);
    BOOST_REQUIRE_EQUAL(sstring("ababa").find_last_of('x'), sstring::npos);
    BOOST_REQUIRE_EQUAL(sstring("").find_last_of('a'), sstring::npos);
}


BOOST_AUTO_TEST_CASE(test_append) {
    BOOST_REQUIRE_EQUAL(sstring("aba").append("1234", 3), "aba123");
    BOOST_REQUIRE_EQUAL(sstring("aba").append("1234", 4), "aba1234");
    BOOST_REQUIRE_EQUAL(sstring("aba").append("1234", 0), "aba");
}

BOOST_AUTO_TEST_CASE(test_replace) {
    BOOST_REQUIRE_EQUAL(sstring("abc").replace(1,1, "xyz", 1), "axc");
    BOOST_REQUIRE_EQUAL(sstring("abc").replace(3,2, "xyz", 2), "abcxy");
    BOOST_REQUIRE_EQUAL(sstring("abc").replace(2,2, "xyz", 2), "abxy");
    BOOST_REQUIRE_EQUAL(sstring("abc").replace(0,2, "", 0), "c");
    BOOST_REQUIRE_THROW(sstring("abc").replace(4,1, "xyz", 1), std::out_of_range);
    const char* s = "xyz";
    sstring str("abcdef");
    BOOST_REQUIRE_EQUAL(str.replace(str.begin() + 1 , str.begin() + 3, s + 1, s + 3), "ayzdef");
    BOOST_REQUIRE_THROW(sstring("abc").replace(4,1, "xyz", 1), std::out_of_range);

}

BOOST_AUTO_TEST_CASE(test_insert) {
    sstring str("abc");
    const char* s = "xyz";
    str.insert(str.begin() +1, s + 1, s + 2);
    BOOST_REQUIRE_EQUAL(str, "aybc");
    str = "abc";
    BOOST_REQUIRE_THROW(str.insert(str.begin() + 5, s + 1, s + 2), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(test_erase) {
    sstring str("abcdef");
    auto i = str.erase(str.begin() + 1, str.begin() + 3);
    BOOST_REQUIRE_EQUAL(*i, 'd');
    BOOST_REQUIRE_EQUAL(str, "adef");
}

BOOST_AUTO_TEST_CASE(test_ctor_iterator) {
    std::list<char> data{{'a', 'b', 'c'}};
    sstring s(data.begin(), data.end());
    BOOST_REQUIRE_EQUAL(s, "abc");
}

BOOST_AUTO_TEST_CASE(test_nul_termination) {
    using stype = basic_sstring<char, uint32_t, 15, true>;

    for (int size = 1; size <= 32; size *= 2) {
        auto s1 = uninitialized_string<stype>(size - 1);
        BOOST_REQUIRE_EQUAL(s1.c_str()[size - 1], '\0');
        auto s2 = uninitialized_string<stype>(size);
        BOOST_REQUIRE_EQUAL(s2.c_str()[size], '\0');

        s1 = stype("01234567890123456789012345678901", size - 1);
        BOOST_REQUIRE_EQUAL(s1.c_str()[size - 1], '\0');
        s2 = stype("01234567890123456789012345678901", size);
        BOOST_REQUIRE_EQUAL(s2.c_str()[size], '\0');

        s1 = stype(size - 1, ' ');
        BOOST_REQUIRE_EQUAL(s1.c_str()[size - 1], '\0');
        s2 = stype(size, ' ');
        BOOST_REQUIRE_EQUAL(s2.c_str()[size], '\0');

        s2 = s1;
        BOOST_REQUIRE_EQUAL(s2.c_str()[s1.size()], '\0');
        s2.resize(s1.size());
        BOOST_REQUIRE_EQUAL(s2.c_str()[s1.size()], '\0');
        BOOST_REQUIRE_EQUAL(s1, s2);

        auto new_size = size / 2;
        s2 = s1;
        s2.resize(new_size);
        BOOST_REQUIRE_EQUAL(s2.c_str()[new_size], '\0');
        BOOST_REQUIRE(!strncmp(s1.c_str(), s2.c_str(), new_size));

        new_size = size * 2;
        s2 = s1;
        s2.resize(new_size);
        BOOST_REQUIRE_EQUAL(s2.c_str()[new_size], '\0');
        BOOST_REQUIRE(!strncmp(s1.c_str(), s2.c_str(), std::min(s1.size(), s2.size())));

        new_size = size * 2;
        s2 = s1 + s1;
        BOOST_REQUIRE_EQUAL(s2.c_str()[s2.size()], '\0');
        BOOST_REQUIRE(!strncmp(s1.c_str(), s2.c_str(), std::min(s1.size(), s2.size())));
    }
}

BOOST_AUTO_TEST_CASE(test_resize_and_overwrite) {
    static constexpr size_t new_size = 42;
    static constexpr char pattern = 's';
    {
        // the size of new content is identical to the specified count
        sstring s;
        s.resize_and_overwrite(new_size, [](char* buf, size_t n) {
            memset(buf, pattern, n);
            return n;
        });
        BOOST_CHECK_EQUAL(s, sstring(new_size, pattern));
    }
    {
        // the size of new content is smaller than the specified count
        static constexpr size_t smaller_size = new_size / 2;
        sstring s;
        s.resize_and_overwrite(new_size, [](char* buf, size_t n) {
            memset(buf, pattern, smaller_size);
            return smaller_size;
        });
        BOOST_CHECK_EQUAL(s, sstring(smaller_size, pattern));
    }
}
