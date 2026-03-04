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

#define BOOST_TEST_MODULE core

#include <numeric>
#include <sys/uio.h>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <boost/test/unit_test.hpp>
#include <seastar/core/sstring.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/util/internal/iovec_utils.hh>

using namespace seastar;

static void do_test_detach_buffers(size_t initial_buffers, std::vector<size_t> initial_sizes) {
    std::vector<temporary_buffer<char>> bufs;
    bufs.reserve(initial_buffers);
    char letter = 'a';
    size_t total_len = 0;
    for (auto size : initial_sizes) {
        bufs.emplace_back(temporary_buffer<char>::copy_of(sstring(size, letter++)));
        total_len += size;
    }

    auto show_buffers = [] (const std::vector<temporary_buffer<char>>& bufs) {
        for (auto& b : bufs) {
            fmt::print(" [{}]", internal::to_sstring<sstring>(b));
        }
    };

    fmt::print("Detaching from {} buffers ({} chars):", initial_buffers, total_len);
    show_buffers(bufs);
    fmt::print("\n");

    auto merge_buffers = [] (const std::vector<temporary_buffer<char>>& bufs) {
        size_t len = 0;
        for (auto& b : bufs) {
            BOOST_REQUIRE_NE(b.size(), 0);
            len += b.size();
        }
        temporary_buffer<char> res(len);
        len = 0;
        for (auto& b : bufs) {
            std::copy_n(b.get(), b.size(), res.get_write() + len);
            len += b.size();
        }
        return res;
    };

    auto bufs_m = merge_buffers(bufs);

    for (size_t off = 1; off < total_len; off++) {
        std::vector<temporary_buffer<char>> copy_of_bufs;
        copy_of_bufs.reserve(bufs.size());
        for (auto& b : bufs) {
            copy_of_bufs.emplace_back(b.get(), b.size());
        }

        auto res = internal::detach_front(copy_of_bufs, off);

        fmt::print("/{} -> {}/{}:", off, res.size(), copy_of_bufs.size());
        show_buffers(res);
        fmt::print(" +");
        show_buffers(copy_of_bufs);
        fmt::print("\n");

        for (auto& b : copy_of_bufs) {
            res.emplace_back(std::move(b));
        }

        auto res_m = merge_buffers(res);
        BOOST_REQUIRE(res_m == bufs_m);
    }
}

static void do_test_detach_buffers(std::vector<size_t> sizes) {
    for (size_t s : {1, 3, 8}) {
        sizes.back() = s;
        do_test_detach_buffers(sizes.size(), sizes);

        if (sizes.size() < 4) {
            auto copy_of_sizes = sizes;
            copy_of_sizes.emplace_back(0);
            do_test_detach_buffers(std::move(copy_of_sizes));
        }
    }
}

BOOST_AUTO_TEST_CASE(test_detach_buffers) {
    do_test_detach_buffers({0});
}

BOOST_AUTO_TEST_CASE(test_iovec_trim_front) {
    const char* data = "abcdefghijklmno";

    for (size_t l = 0; l < 17; l++) {
        std::vector<iovec> iovs;
        iovs.push_back(iovec{ (void*)(data + 0), 5 });
        iovs.push_back(iovec{ (void*)(data + 5), 3 });
        iovs.push_back(iovec{ (void*)(data + 8), 7 });
        auto res = internal::iovec_trim_front(std::span(iovs), l);
        if (l >= 15) {
            BOOST_REQUIRE(res.empty());
        } else {
            BOOST_REQUIRE(res.size() > 0);
            BOOST_REQUIRE_EQUAL(*reinterpret_cast<char*>(res[0].iov_base), data[l]);
            BOOST_REQUIRE_NE(res[0].iov_len, 0);
            size_t total = std::accumulate(res.begin(), res.end(), size_t(0), [] (size_t s, const auto& b) { return s + b.iov_len; });
            BOOST_REQUIRE_EQUAL(total, 15 - l);
            if (res.size() > 1) {
                BOOST_REQUIRE_EQUAL(*reinterpret_cast<char*>(res[1].iov_base), data[l + res[0].iov_len]);
            }
        }
    }
}
