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
 * Copyright (C) 2020 ScyllaDB Ltd.
 */


#include <boost/container/deque.hpp>
#include <boost/container/options.hpp>
#include <seastar/testing/perf_tests.hh>
#include <seastar/core/chunked_fifo.hh>
#include <seastar/core/circular_buffer.hh>

using trivial_elem = int;

static constexpr size_t big_size = 10000;
static constexpr size_t small_size = 3;

// test should do this many iterations, at least, inside the "measured region"
// to defray the cost of the start/stop measuring time pairs, which are expensive
static constexpr size_t total_iters = 10000;


struct nontrivial_elem {
    nontrivial_elem(int x) : x{x} { perf_tests::do_not_optimize(this->x); }
    nontrivial_elem(const nontrivial_elem&) = default;
    nontrivial_elem& operator=(const nontrivial_elem&) = default;
    ~nontrivial_elem() {
        perf_tests::do_not_optimize(x);
    }
    int x;
};

struct fifo_traits {
    template <typename T>
    using type = chunked_fifo<T>;
};

struct circ_traits {
    template <typename T>
    using type = circular_buffer<T>;
};

struct boost_deque_traits {
    using deque_opts = boost::container::deque_options_t<boost::container::block_bytes<16384u>>;

    template <typename T>
    using type = boost::container::deque<T, void, deque_opts>;
};

template <typename C>
    requires requires(C c) { c.reserve(0); }
void reserve(C& c, size_t size) {
    c.reserve(size);
}

template <typename C>
void reserve(C& c, size_t size) {}

template <typename Traits, typename T>
auto make_n(size_t size) {
    using container = Traits::template type<T>;

    perf_tests::do_not_optimize(size);

    container c;
    reserve(c, size);

    for (size_t i = 0; i < size; i++) {
        auto e = trivial_elem(i);
        c.push_back(e);
    }

    perf_tests::do_not_optimize(c);

    return c;
}

struct container_perf {};


size_t outer_loops(size_t inner_size) {
    return total_iters / inner_size + 1;
}

template <typename Traits>
size_t iteration_bench(size_t size) {
    auto c = make_n<Traits, trivial_elem>(size);
    auto outer = outer_loops(size);

    perf_tests::start_measuring_time();
    for (size_t o = 0; o < outer; o++) {
        for (auto& e : c) {
            perf_tests::do_not_optimize(e);
        }
    }
    perf_tests::stop_measuring_time();
    return outer * size;
}

template <typename Traits>
size_t index_bench(size_t size) {
    auto c = make_n<Traits, trivial_elem>(size);
    auto outer = outer_loops(size);

    perf_tests::start_measuring_time();
    for (size_t o = 0; o < outer; o++) {
        for (size_t i = 0; i < size; i++) {
            perf_tests::do_not_optimize(c[i]);
        }
    }
    perf_tests::stop_measuring_time();
    return size * outer;
}

template <typename Traits, typename T>
size_t clear_bench(size_t size) {
    auto c = make_n<Traits, T>(size);

    perf_tests::start_measuring_time();
    c.clear();
    perf_tests::stop_measuring_time();

    return size;
}

template <typename Traits, typename T>
size_t erase_bench(size_t size) {
    auto c = make_n<Traits, T>(size);

    perf_tests::start_measuring_time();
    c.erase(c.begin(), c.begin() + size / 2);
    perf_tests::stop_measuring_time();

    perf_tests::do_not_optimize(c);

    return size / 2;
}

template <typename Traits, typename T>
size_t erase_front_bench(size_t size) {
    auto c = make_n<Traits, T>(size);

    perf_tests::start_measuring_time();
    c.pop_front_n(size / 2);
    perf_tests::stop_measuring_time();

    perf_tests::do_not_optimize(c);

    return size / 2;
}

PERF_TEST_F(container_perf, erase_trivial_chunked_fifo) {
    return erase_front_bench<fifo_traits, trivial_elem>(big_size);
}

PERF_TEST_F(container_perf, erase_trivial_circular_buffer) {
    return erase_bench<circ_traits, trivial_elem>(big_size);
}

PERF_TEST_F(container_perf, erase_trivial_boost_deque) {
    return erase_bench<boost_deque_traits, trivial_elem>(big_size);
}

PERF_TEST_F(container_perf, clear_nontrivial_chunked_fifo) {
    return clear_bench<fifo_traits, nontrivial_elem>(big_size);
}

PERF_TEST_F(container_perf, clear_nontrivial_circular_buffer) {
    return clear_bench<circ_traits, nontrivial_elem>(big_size);
}

PERF_TEST_F(container_perf, clear_nontrivial_boost_deque) {
    return clear_bench<boost_deque_traits, nontrivial_elem>(big_size);
}

PERF_TEST_F(container_perf, clear_trivial_chunked_fifo) {
    return clear_bench<fifo_traits, trivial_elem>(big_size);
}

PERF_TEST_F(container_perf, clear_trivial_circular_buffer) {
    return clear_bench<circ_traits, trivial_elem>(big_size);
}

PERF_TEST_F(container_perf, clear_trivial_boost_deque) {
    return clear_bench<boost_deque_traits, trivial_elem>(big_size);
}

PERF_TEST_F(container_perf, iter_big_chunked_fifo) {
    return iteration_bench<fifo_traits>(big_size);
}

PERF_TEST_F(container_perf, iter_big_circular_buffer) {
    return iteration_bench<circ_traits>(big_size);
}

PERF_TEST_F(container_perf, iter_big_boost_deque) {
    return iteration_bench<boost_deque_traits>(big_size);
}

PERF_TEST_F(container_perf, index_big_circular_buffer) {
    return index_bench<circ_traits>(big_size);
}

PERF_TEST_F(container_perf, index_big_boost_deque) {
    return index_bench<boost_deque_traits>(big_size);
}

PERF_TEST_F(container_perf, iter_small_chunked_fifo) {
    return iteration_bench<fifo_traits>(small_size);
}

PERF_TEST_F(container_perf, iter_small_circular_buffer) {
    return iteration_bench<circ_traits>(small_size);
}

PERF_TEST_F(container_perf, iter_small_boost_deque) {
    return iteration_bench<boost_deque_traits>(small_size);
}

