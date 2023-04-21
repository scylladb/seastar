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
 * Copyright (C) 2023 ScyllaDB
 */

#include <seastar/testing/perf_tests.hh>

#include <seastar/core/memory.hh>
#include <sys/mman.h>

struct allocator_test {
    static constexpr size_t COUNT = 1000;
    std::array<void *, COUNT> _pointers{};
};

struct allocator_test_split : public allocator_test {
    enum alloc_test_flags {
        MEASURE_ALLOC = 1,
        MEASURE_FREE = 2,
    };

    template <size_t alloc_size, alloc_test_flags F = alloc_test_flags(MEASURE_ALLOC |
                                                    MEASURE_FREE)>
    size_t alloc_test() {
        // in some cases we want to measure allocation, on deallocation or
        // both, and this helper facilitates that with a minimum of fuss
        // it always executes fn on every pointer in the array, but only
        // measures when should_measure is true
        auto run_maybe_measure = [this](bool should_measure, auto fn) {
            if (should_measure) {
                perf_tests::start_measuring_time();
            }
            for (auto &p : _pointers) {
                fn(p);
            }
            if (should_measure) {
                perf_tests::stop_measuring_time();
            }
        };

        run_maybe_measure(F & MEASURE_ALLOC, [](auto& p) { p = std::malloc(alloc_size); });
        run_maybe_measure(F & MEASURE_FREE, [](auto& p) { std::free(p); });
        return _pointers.size();
    }
};

PERF_TEST_F(allocator_test_split, alloc_only) { return alloc_test<8, MEASURE_ALLOC>(); }

PERF_TEST_F(allocator_test_split, free_only) { return alloc_test<8, MEASURE_FREE>(); }

PERF_TEST_F(allocator_test_split, alloc_free) { return alloc_test<8>(); }

// this test doesn't serve much value. It should take about 10 times as the
// single alloc test above. If not, something is wrong.
PERF_TEST_F(allocator_test, single_alloc_and_free_small_many)
{
    const std::size_t allocs = 10;

    for (std::size_t i = 0; i < allocs; ++i) {
        auto ptr = malloc(10);
        perf_tests::do_not_optimize(ptr);
        _pointers[i] = ptr;
    }

    for (std::size_t i = 0; i < allocs; ++i) {
        free(_pointers[i]);
    }
}

// Allocate from more than one page. Should also not suffer a perf penalty 
// As we should have at least min free of 50 objects (hard coded internally)
PERF_TEST_F(allocator_test, single_alloc_and_free_small_many_cross_page)
{
    const std::size_t alloc_size = 1024;
    const std::size_t allocs = (seastar::memory::page_size / alloc_size) + 1;

    for (std::size_t i = 0; i < allocs; ++i) {
        auto ptr = malloc(alloc_size);
        perf_tests::do_not_optimize(ptr);
        _pointers[i] = ptr;
    }

    for (std::size_t i = 0; i < allocs; ++i) {
        free(_pointers[i]);
    }
}

// Include an allocation in the benchmark that will require going to the large pool
// for more data for the small pool
PERF_TEST_F(allocator_test, single_alloc_and_free_small_many_cross_page_alloc_more)
{
    const std::size_t alloc_size = 1024;
    const std::size_t allocs = 101; // at 1024 alloc size we will have a _max_free of 100 objects

    for (std::size_t i = 0; i < allocs; ++i) {
        auto ptr = malloc(alloc_size);
        perf_tests::do_not_optimize(ptr);
        _pointers[i] = ptr;
    }

    for (std::size_t i = 0; i < allocs; ++i) {
        free(_pointers[i]);
    }
}

PERF_TEST_F(allocator_test_split, alloc_only_large) { return alloc_test<32000, MEASURE_ALLOC>(); }

PERF_TEST_F(allocator_test_split, free_only_large) { return alloc_test<32000, MEASURE_FREE>(); }

PERF_TEST_F(allocator_test_split, alloc_free_large) { return alloc_test<32000>(); }
