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

#include <seastar/core/memory.hh>
#include <seastar/core/shard_id.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/testing/perf_tests.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/util/log.hh>
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

SEASTAR_TEST_CASE(new_0) {

    {
        // new must always return a non-null pointer, even for 0 size
        auto obj = operator new(0);
        BOOST_REQUIRE(obj != nullptr);
        operator delete(obj);
    }

    {
        // same test but with a zero length array
        auto obj = new char[0];
        BOOST_REQUIRE(obj != nullptr);
        delete [] obj;
    }

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

#ifdef __cpp_sized_deallocation
SEASTAR_TEST_CASE(test_sized_delete) {
    for (size_t size = 0; size <= 65536; size++) {
        void *p0 = operator new(size), *p1 = operator new(size);
        BOOST_REQUIRE(p0 != nullptr);
        BOOST_REQUIRE(p1 != nullptr);
        ::memset(p0, 1, size);
        ::memset(p1, 2, size);
        perf_tests::do_not_optimize(p0);
        perf_tests::do_not_optimize(p1);
        operator delete(p0, size);
        operator delete(p1, size);
    }
    return make_ready_future<>();
}
#endif

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

SEASTAR_THREAD_TEST_CASE(test_cross_thread_realloc) {
    // Tests that realloc seems to do the right thing with various sizes of
    // buffer, including cases where the initial allocation is on another
    // shard.
    // Needs at least 2 shards to usefully test the cross-shard aspect but
    // still passes when only 1 shard is used.
    auto do_xshard_realloc = [](bool cross_shard, size_t initial_size, size_t realloc_size) {
        BOOST_TEST_CONTEXT("cross_shard=" << cross_shard << ", initial="
                << initial_size << ", realloc_size=" << realloc_size) {

            auto other_shard = (this_shard_id() + cross_shard) % smp::count;

            char *p = static_cast<char *>(malloc(initial_size));

            // write some sentinels and check them after realloc
            // x start of region
            // y end of realloc'd region (if it falls within the initial size)
            // z end of initial region
            if (initial_size > 0) {
                p[0] = 'x';
                p[initial_size - 1] = 'z';
                if (realloc_size > 0 && realloc_size <= initial_size) {
                    p[realloc_size - 1] = 'y';
                }
            }
            smp::submit_to(other_shard, [=] {
                char* p2 = static_cast<char *>(realloc(p, realloc_size));
                if (initial_size > 0 && realloc_size > 0) {
                    BOOST_REQUIRE_EQUAL(p2[0], 'x');
                    if (realloc_size <= initial_size) {
                        BOOST_REQUIRE_EQUAL(p2[realloc_size - 1], 'y');
                    }
                    if (realloc_size > initial_size) {
                        BOOST_REQUIRE_EQUAL(p2[initial_size - 1], 'z');
                    }
                }
                free(p2);
            }).get();
        }
    };

    for (auto& cross_shard : {false, true}) {
        do_xshard_realloc(cross_shard, 0, 0);
        do_xshard_realloc(cross_shard, 0, 1);
        do_xshard_realloc(cross_shard, 1, 0);
        do_xshard_realloc(cross_shard, 50, 100);
        do_xshard_realloc(cross_shard, 100, 50);
        do_xshard_realloc(cross_shard, 100000, 500000);
        do_xshard_realloc(cross_shard, 500000, 100000);
    }
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

SEASTAR_TEST_CASE(test_enable_abort_on_oom) {
    bool original = seastar::memory::is_abort_on_allocation_failure();

    seastar::memory::set_abort_on_allocation_failure(false);
    BOOST_CHECK(!seastar::memory::is_abort_on_allocation_failure());

    seastar::memory::set_abort_on_allocation_failure(true);
    BOOST_CHECK(seastar::memory::is_abort_on_allocation_failure());

    seastar::memory::set_abort_on_allocation_failure(original);

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

    // test that new[] throws
    stats = seastar::memory::stats();
    BOOST_REQUIRE_THROW(sink = new char[size], std::bad_alloc);
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

template <typename Func>
requires requires (Func fn) { fn(); }
void check_function_allocation(const char* name, size_t expected_allocs, Func f) {
    auto before = seastar::memory::stats();
    f();
    auto after = seastar::memory::stats();

    BOOST_TEST_INFO("After function: " << name);
    BOOST_REQUIRE_EQUAL(expected_allocs, after.mallocs() - before.mallocs());
}

SEASTAR_TEST_CASE(test_diagnostics_allocation) {

    check_function_allocation("empty", 0, []{});

    check_function_allocation("operator new", 1, []{
        // note that many pairs of malloc/free-alikes can just be optimized
        // away, but not operator new(size_t), per the standard
        void * volatile p = operator new(1);
        operator delete(p);
    });

    // The meat of this test. Dump the diagnostics report to the log and ensure it
    // doesn't allocate. Doing it lots is important because it may alloc only occasionally:
    // a real example being the optimized timestamp logging which (used to) make an allocation
    // only once a second.
    check_function_allocation("log_memory_diagnostics_report", 0, [&]{
        for (int i = 0; i < 1000; i++) {
            seastar::memory::internal::log_memory_diagnostics_report(log_level::info);
        }
    });

    return seastar::make_ready_future();
}

#ifdef SEASTAR_HEAPPROF

// small wrapper to disincentivize gcc from unrolling the loop
[[gnu::noinline]]
char* malloc_wrapper(size_t size) {
    auto ret = static_cast<char*>(malloc(size));
    *ret = 'c'; // to prevent compiler from considering this a dead allocation and optimizing it out
    return ret;
}

namespace seastar::memory {
std::ostream& operator<<(std::ostream& os, const allocation_site& site) {
    os << "allocation_site[count: " << site.count << ", size: " << site.size << "]";
    return os;
}
}

SEASTAR_TEST_CASE(test_sampled_profile_collection_small)
{
    {
        auto stats = seastar::memory::sampled_memory_profile();
        BOOST_REQUIRE_EQUAL(stats.size(), 0);
    }

    std::size_t count = 100;
    std::vector<volatile char*> ptrs(count);

    seastar::memory::set_heap_profiling_sampling_rate(100);

#ifdef __clang__
    #pragma nounroll
#endif
    for (std::size_t i = 0; i < count / 2; ++i) {
        ptrs[i] = malloc_wrapper(10);
    }

#ifdef __clang__
    #pragma nounroll
#endif
    for (std::size_t i = count / 2; i < count; ++i) {
        ptrs[i] = malloc_wrapper(10);
    }

    auto get_samples = []() {
        auto stats0 = seastar::memory::sampled_memory_profile();
        auto stats1 = seastar::memory::sampled_memory_profile();

        // two back-to-back copies of the sample should have the same value
        BOOST_CHECK_EQUAL(stats0, stats1);

        // check that we get the same value from the raw array iterface
        std::vector<seastar::memory::allocation_site> stats2(stats0.size());
        auto sz2 = seastar::memory::sampled_memory_profile(stats2.data(), stats2.size());
        BOOST_CHECK_EQUAL(stats0.size(), sz2);
        BOOST_CHECK_EQUAL(stats0, stats2);

        // check with +1 size, we expect to still only get size elements
        std::vector<seastar::memory::allocation_site> stats3(stats0.size() + 1);
        auto sz3 = seastar::memory::sampled_memory_profile(stats3.data(), stats3.size());
        BOOST_CHECK_EQUAL(stats0.size(), sz3);
        stats3.resize(sz3);
        BOOST_CHECK_EQUAL(stats0, stats3);

        return stats0;
    };

    // NB: the test framework allocates
    seastar::memory::set_heap_profiling_sampling_rate(0);

    {
        auto stats = get_samples();
        BOOST_REQUIRE_EQUAL(stats.size(), 2);
        BOOST_REQUIRE_EQUAL(stats[0].size, stats[0].count * 100);
    }

    seastar::memory::set_heap_profiling_sampling_rate(100);

    for (auto ptr : ptrs) {
        free((void*)ptr);
    }

    seastar::memory::set_heap_profiling_sampling_rate(0);

    {
        auto stats = get_samples();
        BOOST_REQUIRE_EQUAL(stats.size(), 0);
    }

    return seastar::make_ready_future();
}

SEASTAR_TEST_CASE(test_sampled_profile_collection_large)
{
    {
        auto stats = seastar::memory::sampled_memory_profile();
        BOOST_REQUIRE_EQUAL(stats.size(), 0);
    }

    std::size_t count = 100;
    std::vector<volatile char*> ptrs(count);

    seastar::memory::set_heap_profiling_sampling_rate(1000000);

#ifdef __clang__
    #pragma nounroll
#endif
    for (std::size_t i = 0; i < count / 2; ++i) {
        ptrs[i] = malloc_wrapper(100000);
    }

#ifdef __clang__
    #pragma nounroll
#endif
    for (std::size_t i = count / 2; i < count; ++i) {
        ptrs[i] = malloc_wrapper(100000);
    }

    // NB: the test framework allocate
    seastar::memory::set_heap_profiling_sampling_rate(0);

    {
        auto stats = seastar::memory::sampled_memory_profile();
        BOOST_REQUIRE_EQUAL(stats.size(), 2);
        BOOST_REQUIRE_EQUAL(stats[0].size, stats[0].count * 1000000);
    }

    seastar::memory::set_heap_profiling_sampling_rate(1000000);

    for (auto ptr : ptrs) {
        free((void*)ptr);
    }

    seastar::memory::set_heap_profiling_sampling_rate(0);

    {
        auto stats = seastar::memory::sampled_memory_profile();
        // NOTE this is because right now the tracking structure doesn't delete call sites ever
        BOOST_REQUIRE_EQUAL(stats.size(), 0);
    }

    return seastar::make_ready_future();
}

SEASTAR_TEST_CASE(test_sampled_profile_collection_max_sites)
{
    std::size_t count = 1010;
    std::vector<volatile char*> ptrs(count);

    seastar::memory::set_heap_profiling_sampling_rate(100);

    #pragma GCC unroll 1010
    for (std::size_t i = 0; i < count; ++i) {
        volatile char* ptr = static_cast<char*>(malloc(1000));
        *ptr = 'c'; // to prevent compiler from considering this a dead allocation and optimizing it out
        ptrs[i] = ptr;
    }

    seastar::memory::set_heap_profiling_sampling_rate(0);

    {
        auto stats = seastar::memory::sampled_memory_profile();
        BOOST_REQUIRE_EQUAL(stats.size(), 1000);
    }

    for (auto ptr : ptrs) {
        free((void*)ptr);
    }

    return seastar::make_ready_future();
}

SEASTAR_TEST_CASE(test_change_sample_rate)
{
    {
        auto stats = seastar::memory::sampled_memory_profile();
        BOOST_REQUIRE_EQUAL(stats.size(), 0);
    }

    std::size_t sample_rate = 100;
    std::size_t count = 10000;
    std::vector<volatile char*> ptrs(count);

    seastar::memory::set_heap_profiling_sampling_rate(sample_rate);

#ifdef __clang__
    #pragma nounroll
#endif
    for (std::size_t i = 0; i < count; ++i) {
        ptrs[i] = malloc_wrapper(10);
    }

    // NB: the test framework allocates
    seastar::memory::set_heap_profiling_sampling_rate(0);

    size_t last_alloc_size = 0;
    {
        auto stats = seastar::memory::sampled_memory_profile();
        BOOST_REQUIRE_EQUAL(stats.size(), 1);
        last_alloc_size = stats[0].size;
        BOOST_REQUIRE_EQUAL(stats[0].size, stats[0].count * sample_rate);
    }

    seastar::memory::set_heap_profiling_sampling_rate(sample_rate);

    size_t free_iter = 0;
    // free some of the allocations to check size changes
    for (size_t i = 0; i < count / 4; ++i, ++free_iter) {
        free((void*)ptrs[free_iter]);
    }

    seastar::memory::set_heap_profiling_sampling_rate(0);

    {
        auto stats = seastar::memory::sampled_memory_profile();
        BOOST_REQUIRE_EQUAL(stats.size(), 1);
        BOOST_REQUIRE_EQUAL(stats[0].size, stats[0].count * sample_rate);
        BOOST_REQUIRE_NE(stats[0].size, last_alloc_size);
        BOOST_REQUIRE_GT(stats[0].size, 0);
        last_alloc_size = stats[0].size;
    }

    // now increase the sampling rate with outstanding allocations from the old rate
    seastar::memory::set_heap_profiling_sampling_rate(sample_rate * 100);

    for (size_t i = 0; i < count / 4; ++i, ++free_iter) {
        free((void*)ptrs[free_iter]);
    }

    seastar::memory::set_heap_profiling_sampling_rate(0);

    {
        auto stats = seastar::memory::sampled_memory_profile();
        BOOST_REQUIRE_EQUAL(stats.size(), 1);
        BOOST_REQUIRE_LT(stats[0].size, last_alloc_size); // should not have underflowed
    }

    seastar::memory::set_heap_profiling_sampling_rate(sample_rate);

    // free the rest
    for (size_t i = 0; i < count / 2; ++i, ++free_iter) {
        free((void*)ptrs[free_iter]);
    }

    seastar::memory::set_heap_profiling_sampling_rate(0);

    {
        auto stats = seastar::memory::sampled_memory_profile();
        BOOST_REQUIRE_EQUAL(stats.size(), 0);
    }

    return seastar::make_ready_future();
}


#endif // SEASTAR_HEAPPROF

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
