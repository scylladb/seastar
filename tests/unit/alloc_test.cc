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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#include <seastar/testing/test_case.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/temporary_buffer.hh>
#include <vector>

using namespace seastar;

SEASTAR_TEST_CASE(alloc_almost_all_and_realloc_it_with_a_smaller_size) {
#ifndef SEASTAR_DEFAULT_ALLOCATOR
    auto all = memory::stats().total_memory();
    auto reserve = size_t(0.02 * all);
    auto to_alloc = all - (reserve + (10 << 20));
    auto orig_to_alloc = to_alloc;
    auto obj = malloc(to_alloc);
    while (!obj) {
        to_alloc *= 0.9;
        obj = malloc(to_alloc);
    }
    BOOST_REQUIRE(to_alloc > orig_to_alloc / 4);
    BOOST_REQUIRE(obj != nullptr);
    auto obj2 = realloc(obj, to_alloc - (1 << 20));
    BOOST_REQUIRE(obj == obj2);
    free(obj2);
#endif
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(malloc_0_and_free_it) {
#ifndef SEASTAR_DEFAULT_ALLOCATOR
    auto obj = malloc(0);
    BOOST_REQUIRE(obj != nullptr);
    free(obj);
#endif
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_live_objects_counter_with_cross_cpu_free) {
    return smp::submit_to(1, [] {
        auto ret = std::vector<std::unique_ptr<bool>>(1000000);
        for (auto& o : ret) {
            o = std::make_unique<bool>(false);
        }
        return ret;
    }).then([] (auto&& vec) {
        vec.clear(); // cause cross-cpu free
        BOOST_REQUIRE(memory::stats().live_objects() < std::numeric_limits<size_t>::max() / 2);
    });
}

SEASTAR_TEST_CASE(test_aligned_alloc) {
    for (size_t align = sizeof(void*); align <= 65536; align <<= 1) {
        for (size_t size = align; size <= align * 2; size <<= 1) {
            void *p = aligned_alloc(align, size);
            BOOST_REQUIRE(p != nullptr);
            BOOST_REQUIRE((reinterpret_cast<uintptr_t>(p) % align) == 0);
            ::memset(p, 0, size);
            free(p);
        }
    }
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_temporary_buffer_aligned) {
    for (size_t align = sizeof(void*); align <= 65536; align <<= 1) {
        for (size_t size = align; size <= align * 2; size <<= 1) {
            auto buf = temporary_buffer<char>::aligned(align, size);
            void *p = buf.get_write();
            BOOST_REQUIRE(p != nullptr);
            BOOST_REQUIRE((reinterpret_cast<uintptr_t>(p) % align) == 0);
            ::memset(p, 0, size);
        }
    }
    return make_ready_future<>();
}
