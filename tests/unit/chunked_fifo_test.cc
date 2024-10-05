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
 * Copyright (C) 2016 ScyllaDB Ltd.
 */


#define BOOST_TEST_MODULE core

#include <boost/test/tools/context.hpp>

#include <boost/test/unit_test.hpp>
#include <seastar/core/chunked_fifo.hh>
#include <seastar/core/circular_buffer.hh>
#include <stdlib.h>
#include <chrono>
#include <deque>
#include <iterator>
#include <ranges>
#if __has_include(<version>)
#include <version>
#endif

using namespace seastar;

static_assert(std::weakly_incrementable<chunked_fifo<int>::iterator>);
static_assert(std::weakly_incrementable<chunked_fifo<int>::const_iterator>);
static_assert(std::sentinel_for<chunked_fifo<int>::iterator, chunked_fifo<int>::iterator>);
static_assert(std::sentinel_for<chunked_fifo<int>::const_iterator, chunked_fifo<int>::const_iterator>);

static_assert(std::ranges::range<chunked_fifo<const int>>);

BOOST_AUTO_TEST_CASE(chunked_fifo_small) {
    // Check all the methods of chunked_fifo but with a trivial type (int) and
    // only a few elements - and in particular a single chunk is enough.
    chunked_fifo<int> fifo;
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), true);
    fifo.push_back(3);
    BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), false);
    BOOST_REQUIRE_EQUAL(fifo.front(), 3);
    fifo.push_back(17);
    BOOST_REQUIRE_EQUAL(fifo.size(), 2u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), false);
    BOOST_REQUIRE_EQUAL(fifo.front(), 3);
    fifo.pop_front();
    BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), false);
    BOOST_REQUIRE_EQUAL(fifo.front(), 17);
    fifo.pop_front();
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), true);
    // The previously allocated chunk should have been freed, and now
    // a new one will need to be allocated:
    fifo.push_back(57);
    BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), false);
    BOOST_REQUIRE_EQUAL(fifo.front(), 57);
    // check miscelleneous methods (at least they shouldn't crash)
    fifo.clear();
    fifo.shrink_to_fit();
    fifo.reserve(1);
    fifo.reserve(100);
    fifo.reserve(1280);
    fifo.shrink_to_fit();
    fifo.reserve(1280);
}

BOOST_AUTO_TEST_CASE(chunked_fifo_fullchunk) {
    // Grow a chunked_fifo to exactly fill a chunk, and see what happens when
    // we cross that chunk.
    constexpr size_t N = 128;
    chunked_fifo<int, N> fifo;
    for (int i = 0; i < static_cast<int>(N); i++) {
        fifo.push_back(i);
    }
    BOOST_REQUIRE_EQUAL(fifo.size(), N);
    fifo.push_back(N);
    BOOST_REQUIRE_EQUAL(fifo.size(), N+1);
    for (int i = 0 ; i < static_cast<int>(N+1); i++) {
        BOOST_REQUIRE_EQUAL(fifo.front(), i);
        BOOST_REQUIRE_EQUAL(fifo.size(), N+1-i);
        fifo.pop_front();
    }
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), true);
}

struct trackable_totals {
    size_t cons_called = 0, dtor_called = 0;
};

struct trackable {
    trackable(trackable_totals& tracker) : _tracker{tracker} {
        _tracker.cons_called++;
    }

    trackable(const trackable&) = delete;
    trackable(trackable&&) = delete;
    void operator=(const trackable&) = delete;
    void operator=(trackable&&) = delete;

    ~trackable() {
        _tracker.dtor_called++;
    }

    trackable_totals& _tracker;
};

BOOST_AUTO_TEST_CASE(chunked_fifo_pop_n) {
    trackable_totals ctor_calls;
    constexpr size_t N = 4;
    chunked_fifo<trackable, N / 2> fifo;

    auto fill_and_reset = [&](size_t size) {
        fifo.clear();
        ctor_calls = {};
        for (size_t i = 0; i < size; i++) {
            // we add 2, remove 1 in order to stress the case where chunk::begin and end
            // are outside the range [0, items_per_chunk], i.e., where proper use of mask()
            // is required
            fifo.emplace_back(ctor_calls);
            fifo.emplace_back(ctor_calls);
            fifo.pop_front_n(1);
        }
    };

    for (size_t size : std::views::iota((size_t)0, 2 * N) ) {
        for (size_t pop_count : std::views::iota((size_t)0, size + 1) ) {
            BOOST_TEST_CONTEXT("size: " << size << ", pop_count: " << pop_count) {
                fill_and_reset(size);

                // note that we add 2 and delete 1 element for every element added in
                // fill_and_reset so that affects the numbers below

                BOOST_REQUIRE_EQUAL(fifo.size(), size);
                BOOST_REQUIRE_EQUAL(ctor_calls.cons_called, size * 2);
                BOOST_REQUIRE_EQUAL(ctor_calls.dtor_called, size);

                fifo.pop_front_n(pop_count);

                BOOST_REQUIRE_EQUAL(fifo.size(), size - pop_count);
                BOOST_REQUIRE_EQUAL(ctor_calls.cons_called, size * 2);
                BOOST_REQUIRE_EQUAL(ctor_calls.dtor_called, size + pop_count);

                fifo.emplace_back(ctor_calls);

                BOOST_REQUIRE_EQUAL(fifo.size(), size - pop_count + 1);
                BOOST_REQUIRE_EQUAL(ctor_calls.cons_called, size * 2 + 1);
                BOOST_REQUIRE_EQUAL(ctor_calls.dtor_called, size + pop_count);
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(chunked_fifo_big) {
    // Grow a chunked_fifo to many elements, and see things are working as
    // expected
    chunked_fifo<int> fifo;
    constexpr size_t N = 100'000;
    for (int i=0; i < static_cast<int>(N); i++) {
        fifo.push_back(i);
    }
    BOOST_REQUIRE_EQUAL(fifo.size(), N);
    BOOST_REQUIRE_EQUAL(fifo.empty(), false);
    for (int i = 0 ; i < static_cast<int>(N); i++) {
        BOOST_REQUIRE_EQUAL(fifo.front(), i);
        BOOST_REQUIRE_EQUAL(fifo.size(), N-i);
        fifo.pop_front();
    }
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), true);
}

BOOST_AUTO_TEST_CASE(chunked_fifo_constructor) {
    // Check that chunked_fifo appropriately calls the type's constructor
    // and destructor, and doesn't need anything else.
    struct typ {
        int val;
        unsigned* constructed;
        unsigned* destructed;
        typ(int val, unsigned* constructed, unsigned* destructed)
            : val(val), constructed(constructed), destructed(destructed) {
                ++*constructed;
        }
        ~typ() { ++*destructed; }
    };
    chunked_fifo<typ> fifo;
    unsigned constructed = 0, destructed = 0;
    constexpr unsigned N = 1000;
    for (unsigned i = 0; i < N; i++) {
        fifo.emplace_back(i, &constructed, &destructed);
    }
    BOOST_REQUIRE_EQUAL(fifo.size(), N);
    BOOST_REQUIRE_EQUAL(constructed, N);
    BOOST_REQUIRE_EQUAL(destructed, 0u);
    for (unsigned i = 0 ; i < N; i++) {
        BOOST_REQUIRE_EQUAL(fifo.front().val, static_cast<int>(i));
        BOOST_REQUIRE_EQUAL(fifo.size(), N-i);
        fifo.pop_front();
        BOOST_REQUIRE_EQUAL(destructed, i+1);
    }
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), true);
    // Check that destructing a fifo also destructs the objects it still
    // contains
    constructed = destructed = 0;
    {
        chunked_fifo<typ> fifo;
        for (unsigned i = 0; i < N; i++) {
            fifo.emplace_back(i, &constructed, &destructed);
            BOOST_REQUIRE_EQUAL(fifo.front().val, 0);
            BOOST_REQUIRE_EQUAL(fifo.size(), i+1);
            BOOST_REQUIRE_EQUAL(fifo.empty(), false);
            BOOST_REQUIRE_EQUAL(constructed, i+1);
            BOOST_REQUIRE_EQUAL(destructed, 0u);
        }
    }
    BOOST_REQUIRE_EQUAL(constructed, N);
    BOOST_REQUIRE_EQUAL(destructed, N);
}

BOOST_AUTO_TEST_CASE(chunked_fifo_construct_fail) {
    // Check that if we fail to construct the item pushed, the queue remains
    // empty.
    class my_exception {};
    struct typ {
        typ() {
            throw my_exception();
        }
    };
    chunked_fifo<typ> fifo;
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), true);
    try {
        fifo.emplace_back();
    } catch(my_exception) {
        // expected, ignore
    }
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), true);
}

BOOST_AUTO_TEST_CASE(chunked_fifo_construct_fail2) {
    // A slightly more elaborate test, with a chunk size of 2
    // items, and the third addition failing, so the question is
    // not whether empty() is wrong immediately, but whether after
    // we pop the two items, it will become true or we'll be left
    // with an empty chunk.
    class my_exception {};
    struct typ {
        typ(bool fail) {
            if (fail) {
                throw my_exception();
            }
        }
    };
    chunked_fifo<typ, 2> fifo;
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), true);
    fifo.emplace_back(false);
    fifo.emplace_back(false);
    try {
        fifo.emplace_back(true);
    } catch(my_exception) {
        // expected, ignore
    }
    BOOST_REQUIRE_EQUAL(fifo.size(), 2u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), false);
    fifo.pop_front();
    BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), false);
    fifo.pop_front();
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE_EQUAL(fifo.empty(), true);
}

// Enable the following to run some benchmarks on different queue options
#if 0
// Unfortunately, C++ lacks the trivial feature of converting a type's name,
// in compile time, to a string (akin to the C preprocessor's "#" feature).
// Here is a neat trick to replace it - use typeinfo<T>::name() or
// type_name<T>() to get a constant string name of the type.
#include <cxxabi.h>
template <typename T>
class typeinfo {
private:
    static const char *_name;
public:
    static const char *name() {
        int status;
        if (!_name)
            _name = abi::__cxa_demangle(typeid(T).name(), 0, 0, &status);
        return _name;
    }
};
template<typename T> const char *typeinfo<T>::_name = nullptr;
template<typename T> const char *type_name() {
    return typeinfo<T>::name();
}


template<typename FIFO_TYPE> void
benchmark_random_push_pop() {
    // A test involving a random sequence of pushes and pops. Because the
    // random walk is bounded the 0 end (the queue cannot be popped after
    // being empty), the queue's expected length at the end of the test is
    // not zero.
    // The test uses the same random sequence each time so can be used for
    // benchmarking different queue implementations on the same sequence.
    constexpr int N = 1'000'000'000;
    FIFO_TYPE fifo;
    unsigned int seed = 0;
    int entropy = 0;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; i++) {
        if (!entropy) {
            entropy = rand_r(&seed);
        }
        if (entropy & 1) {
            fifo.push_back(i);
        } else {
            if (!fifo.empty()) {
                fifo.pop_front();
            }
        }
        entropy >>= 1;
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cerr << type_name<FIFO_TYPE>() << ", " << N << " random push-and-pop " << fifo.size() << " " << ms << "ms.\n";
}

template<typename FIFO_TYPE> void
benchmark_push_pop() {
    // A benchmark involving repeated push and then pop to a queue, which
    // will have 0 or 1 items at all times.
    constexpr int N = 1'000'000'000;
    FIFO_TYPE fifo;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; i++) {
        fifo.push_back(1);
        fifo.pop_front();
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cerr << type_name<FIFO_TYPE>() << ", " << N << " push-and-pop " << ms << "ms.\n";
}

template<typename FIFO_TYPE> void
benchmark_push_pop_k() {
    // A benchmark involving repeated pushes of a few items and then popping
    // to a queue, which will have just one chunk (or 0) at all times.
    constexpr int N = 1'000'000'000;
    constexpr int K = 100;
    FIFO_TYPE fifo;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N/K; i++) {
        for(int j = 0; j < K; j++) {
            fifo.push_back(j);
        }
        for(int j = 0; j < K; j++) {
            fifo.pop_front();
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cerr << type_name<FIFO_TYPE>() << ", " << N << " push-and-pop-" << K << " " << ms << "ms.\n";
}

template<typename FIFO_TYPE> void
benchmark_pushes_pops() {
    // A benchmark of pushing a lot of items, and then popping all of them
    constexpr int N = 100'000'000;
    FIFO_TYPE fifo;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < N; i++) {
        fifo.push_back(1);
    }
    for (int i = 0; i < N; i++) {
        fifo.pop_front();
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cerr << type_name<FIFO_TYPE>() << ", " << N << " push-all-then-pop-all " << ms << "ms.\n";
}

template<typename FIFO_TYPE> void
benchmark_all() {
    std::cerr << "\n  --- " << type_name<FIFO_TYPE>() << ": \n";
    benchmark_random_push_pop<FIFO_TYPE>();
    benchmark_push_pop<FIFO_TYPE>();
    benchmark_push_pop_k<FIFO_TYPE>();
    benchmark_pushes_pops<FIFO_TYPE>();
}

BOOST_AUTO_TEST_CASE(chunked_fifo_benchmark) {
    benchmark_all<chunked_fifo<int>>();
    benchmark_all<circular_buffer<int>>();
    benchmark_all<std::deque<int>>();
    benchmark_all<std::list<int>>();
}
#endif

BOOST_AUTO_TEST_CASE(chunked_fifo_iterator) {
    constexpr auto items_per_chunk = 8;
    auto fifo = chunked_fifo<int, items_per_chunk>{};
    auto reference = std::deque<int>{};

    BOOST_REQUIRE(fifo.begin() == fifo.end());

    for (int i = 0; i < items_per_chunk * 4; ++i) {
        fifo.push_back(i);
        reference.push_back(i);
        BOOST_REQUIRE(std::equal(fifo.begin(), fifo.end(), reference.begin(), reference.end()));
    }

    for (int i = 0; i < items_per_chunk * 2; ++i) {
        fifo.pop_front();
        reference.pop_front();
        BOOST_REQUIRE(std::equal(fifo.begin(), fifo.end(), reference.begin(), reference.end()));
    }
}

BOOST_AUTO_TEST_CASE(chunked_fifo_const_iterator) {
    constexpr auto items_per_chunk = 8;
    auto fifo = chunked_fifo<int, items_per_chunk>{};
    auto reference = std::deque<int>{};

    BOOST_REQUIRE(fifo.cbegin() == fifo.cend());

    for (int i = 0; i < items_per_chunk * 4; ++i) {
        fifo.push_back(i);
        reference.push_back(i);
        BOOST_REQUIRE(std::equal(fifo.cbegin(), fifo.cend(), reference.cbegin(), reference.cend()));
    }

    for (int i = 0; i < items_per_chunk * 2; ++i) {
        fifo.pop_front();
        reference.pop_front();
        BOOST_REQUIRE(std::equal(fifo.cbegin(), fifo.cend(), reference.cbegin(), reference.cend()));
    }
}
