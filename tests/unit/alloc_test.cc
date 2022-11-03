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
#include <seastar/core/smp.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/util/memory_diagnostics.hh>

#include <memory>
#include <new>
#include <vector>
#include <future>
#include <iostream>

#include <malloc.h>

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

SEASTAR_TEST_CASE(test_memory_diagnostics) {
    auto report = memory::generate_memory_diagnostics_report();
#ifdef SEASTAR_DEFAULT_ALLOCATOR
    BOOST_REQUIRE(report.length() == 0); // empty report with default allocator
#else
    // since the output format is unstructured text, not much
    // to do except test that we get a non-empty string
    BOOST_REQUIRE(report.length() > 0);
    // useful while debugging diagnostics
    // fmt::print("--------------------\n{}--------------------", report);
#endif
    return make_ready_future<>();
}

#ifndef SEASTAR_DEFAULT_ALLOCATOR

struct thread_alloc_info {
    memory::statistics before;
    memory::statistics after;
    void *ptr;
};

template <typename Func>
thread_alloc_info run_with_stats(Func&& f) {
    return std::async([&f](){
        auto before = seastar::memory::stats();
        void* ptr = f();
        auto after = seastar::memory::stats();
        return thread_alloc_info{before, after, ptr};
    }).get();
}

template <typename Func>
void test_allocation_function(Func f) {
    // alien alloc and free
    auto alloc_info = run_with_stats(f);
    auto free_info = std::async([p = alloc_info.ptr]() {
        auto before = seastar::memory::stats();
        free(p);
        auto after = seastar::memory::stats();
        return thread_alloc_info{before, after, nullptr};
    }).get();

    // there were mallocs
    BOOST_REQUIRE(alloc_info.after.foreign_mallocs() - alloc_info.before.foreign_mallocs() > 0);
    // mallocs balanced with frees
    BOOST_REQUIRE(alloc_info.after.foreign_mallocs() - alloc_info.before.foreign_mallocs() == free_info.after.foreign_frees() - free_info.before.foreign_frees());

    // alien alloc reactor free
    auto info = run_with_stats(f);
    auto before_cross_frees = memory::stats().foreign_cross_frees();
    free(info.ptr);
    BOOST_REQUIRE(memory::stats().foreign_cross_frees() - before_cross_frees == 1);

    // reactor alloc, alien free
    void *p = f();
    auto alien_cross_frees = std::async([p]() {
        auto frees_before = memory::stats().cross_cpu_frees();
        free(p);
        return memory::stats().cross_cpu_frees()-frees_before;
    }).get();
    BOOST_REQUIRE(alien_cross_frees == 1);
}

SEASTAR_TEST_CASE(test_foreign_function_use_glibc_malloc) {
    test_allocation_function([]() ->void * { return malloc(1); });
    test_allocation_function([]() { return realloc(NULL, 10); });
    test_allocation_function([]() {
        auto p = malloc(1);
        return realloc(p, 1000);
    });
    test_allocation_function([]() { return aligned_alloc(4, 1024); });
    return make_ready_future<>();
}

// So the compiler won't optimize the call to realloc(nullptr, size)
// and call malloc directly.
void* test_nullptr = nullptr;

SEASTAR_TEST_CASE(test_realloc_nullptr) {
    auto p0 = realloc(test_nullptr, 8);
    BOOST_REQUIRE(p0 != nullptr);
    BOOST_REQUIRE_EQUAL(realloc(p0, 0), nullptr);

    p0 = realloc(test_nullptr, 0);
    BOOST_REQUIRE(p0 != nullptr);
    auto p1 = malloc(0);
    BOOST_REQUIRE(p1 != nullptr);
    free(p0);
    free(p1);

    return make_ready_future<>();
}

void * volatile sink;

SEASTAR_TEST_CASE(test_bad_alloc_throws) {
    // test that a large allocation throws bad_alloc
    auto stats = seastar::memory::stats();

    // this allocation cannot be satisfied (at least when the seastar
    // allocator is used, which it is for this test)
    size_t size = stats.total_memory() * 2;

    auto failed_allocs = [&stats]() {
        return seastar::memory::stats().failed_allocations() - stats.failed_allocations();
    };

    // test that new throws
    stats = seastar::memory::stats();
    BOOST_REQUIRE_THROW(sink = operator new(size), std::bad_alloc);
    BOOST_CHECK_EQUAL(failed_allocs(), 1);

    // test that huge malloc returns null
    stats = seastar::memory::stats();
    BOOST_REQUIRE_EQUAL(malloc(size), nullptr);
    BOOST_CHECK_EQUAL(failed_allocs(), 1);

    // test that huge realloc on nullptr returns null
    stats = seastar::memory::stats();
    BOOST_REQUIRE_EQUAL(realloc(nullptr, size), nullptr);
    BOOST_CHECK_EQUAL(failed_allocs(), 1);

    // test that huge realloc on an existing ptr returns null
    stats = seastar::memory::stats();
    void *p = malloc(1);
    BOOST_REQUIRE(p);
    void *p2 = realloc(p, size);
    BOOST_REQUIRE_EQUAL(p2, nullptr);
    BOOST_CHECK_EQUAL(failed_allocs(), 1);
    free(p2 ?: p);

    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_diagnostics_failures) {
    // test that an allocation failure is reflected in the diagnostics
    auto stats = seastar::memory::stats();

    size_t size = stats.total_memory() * 2; // cannot be satisfied

    // we expect that the failure is immediately reflected in the diagnostics
    try {
        sink = operator new(size);
    } catch (const std::bad_alloc&) {}

    auto report = memory::generate_memory_diagnostics_report();

    // +1 because we caused one additional hard failure from the allocation above
    auto expected = fmt::format("Hard failures: {}", stats.failed_allocations() + 1);

    if (report.find(expected) == seastar::sstring::npos) {
        BOOST_FAIL(fmt::format("Did not find expected message: {} in\n{}\n", expected, report));
    }

    return seastar::make_ready_future();
}


#endif // #ifndef SEASTAR_DEFAULT_ALLOCATOR

SEASTAR_TEST_CASE(test_large_allocation_warning_off_by_one) {
#ifndef SEASTAR_DEFAULT_ALLOCATOR
    constexpr size_t large_alloc_threshold = 1024*1024;
    seastar::memory::scoped_large_allocation_warning_threshold mtg(large_alloc_threshold);
    BOOST_REQUIRE(seastar::memory::get_large_allocation_warning_threshold() == large_alloc_threshold);
    auto old_large_allocs_count = memory::stats().large_allocations();
    volatile auto obj = (char*)malloc(large_alloc_threshold);
    *obj = 'c'; // to prevent compiler from considering this a dead allocation and optimizing it out

    // Verify large allocation was detected by allocator.
    BOOST_REQUIRE(memory::stats().large_allocations() == old_large_allocs_count+1);

    free(obj);
#endif
    return make_ready_future<>();
}
