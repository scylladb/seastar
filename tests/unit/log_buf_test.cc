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
 * Copyright (C) 2020 Cloudius Systems, Ltd.
 */

#include <seastar/testing/test_case.hh>
#include <seastar/util/log.hh>

using namespace seastar;

SEASTAR_TEST_CASE(log_buf_realloc) {
    std::array<char, 128> external_buf;

    const auto external_buf_ptr = reinterpret_cast<uintptr_t>(external_buf.data());

    internal::log_buf b(external_buf.data(), external_buf.size());

    BOOST_REQUIRE_EQUAL(reinterpret_cast<uintptr_t>(b.data()), external_buf_ptr);

    auto it = b.back_insert_begin();

    for (auto i = 0; i < 128; ++i) {
        *it++ = 'a';
    }

    *it = 'a'; // should trigger realloc

    BOOST_REQUIRE_NE(reinterpret_cast<uintptr_t>(b.data()), reinterpret_cast<uintptr_t>(external_buf.data()));

    const char* p = b.data();
    for (auto i = 0; i < 129; ++i) {
        BOOST_REQUIRE_EQUAL(p[i], 'a');
    }

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(log_buf_insert_iterator_format_to) {
    constexpr size_t size = 128;
    auto external_buf = std::make_unique<char[]>(size);
    auto external_buf_ptr = external_buf.get();
    char str[size + 1];

    internal::log_buf b(external_buf_ptr, size);

    BOOST_REQUIRE_EQUAL(reinterpret_cast<uintptr_t>(b.data()), reinterpret_cast<uintptr_t>(external_buf_ptr));

    auto it = b.back_insert_begin();

    memset(str, 'a', size);
    str[size] = '\0';

    it = fmt::format_to(it, fmt::runtime(str), size);
    BOOST_REQUIRE_EQUAL(reinterpret_cast<uintptr_t>(b.data()), reinterpret_cast<uintptr_t>(external_buf_ptr));

    *it++ = '\n';
    BOOST_REQUIRE_NE(reinterpret_cast<uintptr_t>(b.data()), reinterpret_cast<uintptr_t>(external_buf_ptr));

    memset(str, 'b', size);
    it = fmt::format_to(it, fmt::runtime(str), size);
    *it++ = '\n';

    const char* p = b.data();
    size_t pos = 0;
    for (size_t i = 0; i < size; i++) {
        BOOST_REQUIRE_EQUAL(p[pos++], 'a');
    }
    BOOST_REQUIRE_EQUAL(p[pos++], '\n');
    for (size_t i = 0; i < size; i++) {
        BOOST_REQUIRE_EQUAL(p[pos++], 'b');
    }
    BOOST_REQUIRE_EQUAL(p[pos++], '\n');
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(log_buf_clear) {
    internal::log_buf buf;

    auto it = buf.back_insert_begin();

    fmt::format_to(it, "abcd");

    BOOST_CHECK_EQUAL(buf.view(), "abcd");
    auto cap_before = buf.capacity();
    buf.clear();
    BOOST_CHECK_EQUAL(cap_before, buf.capacity());
    BOOST_CHECK_EQUAL(0, buf.size());

    fmt::format_to(it, "uuvvwwxxyyzz");

    BOOST_CHECK_EQUAL(buf.view(), "uuvvwwxxyyzz");
    cap_before = buf.capacity();
    buf.clear();
    BOOST_CHECK_EQUAL(cap_before, buf.capacity());
    BOOST_CHECK_EQUAL(0, buf.size());

    return make_ready_future<>();
}
