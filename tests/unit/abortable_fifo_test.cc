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
 * Copyright (C) 2022 ScyllaDB
 */

#include <chrono>
#include <exception>

#include <seastar/core/thread.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/core/abortable_fifo.hh>
#include <seastar/core/abort_on_expiry.hh>
#include <seastar/core/manual_clock.hh>
#include <seastar/core/timed_out_error.hh>
#include <seastar/util/later.hh>
#include <boost/range/irange.hpp>

using namespace seastar;
using namespace std::chrono_literals;

SEASTAR_TEST_CASE(test_no_abortable_operations) {
    internal::abortable_fifo<int> fifo;

    BOOST_REQUIRE(fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE(!bool(fifo));

    fifo.push_back(1);

    BOOST_REQUIRE(!fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
    BOOST_REQUIRE(bool(fifo));
    BOOST_REQUIRE_EQUAL(fifo.front(), 1);

    fifo.push_back(2);
    fifo.push_back(3);

    BOOST_REQUIRE(!fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 3u);
    BOOST_REQUIRE(bool(fifo));
    BOOST_REQUIRE_EQUAL(fifo.front(), 1);

    fifo.pop_front();

    BOOST_REQUIRE(!fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 2u);
    BOOST_REQUIRE(bool(fifo));
    BOOST_REQUIRE_EQUAL(fifo.front(), 2);

    fifo.pop_front();

    BOOST_REQUIRE(!fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
    BOOST_REQUIRE(bool(fifo));
    BOOST_REQUIRE_EQUAL(fifo.front(), 3);

    fifo.pop_front();

    BOOST_REQUIRE(fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE(!bool(fifo));

    return make_ready_future<>();
}

SEASTAR_THREAD_TEST_CASE(test_abortable_operations) {
    std::vector<int> expired;
    struct my_expiry {
        std::vector<int>& e;
        void operator()(int& v) noexcept { e.push_back(v); }
    };

    internal::abortable_fifo<int, my_expiry> fifo(my_expiry{expired});
    abort_source as;

    fifo.push_back(1, as);

    BOOST_REQUIRE(!fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
    BOOST_REQUIRE(bool(fifo));
    BOOST_REQUIRE_EQUAL(fifo.front(), 1);

    as.request_abort();
    yield().get();

    BOOST_REQUIRE(fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE(!bool(fifo));
    BOOST_REQUIRE_EQUAL(expired.size(), 1u);
    BOOST_REQUIRE_EQUAL(expired[0], 1);

    expired.clear();
    as = abort_source();

    fifo.push_back(1);
    fifo.push_back(2, as);
    fifo.push_back(3);

    as.request_abort();
    yield().get();

    BOOST_REQUIRE(!fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 2u);
    BOOST_REQUIRE(bool(fifo));
    BOOST_REQUIRE_EQUAL(expired.size(), 1u);
    BOOST_REQUIRE_EQUAL(expired[0], 2);
    BOOST_REQUIRE_EQUAL(fifo.front(), 1);
    fifo.pop_front();
    BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
    BOOST_REQUIRE_EQUAL(fifo.front(), 3);
    fifo.pop_front();
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);

    expired.clear();

    abort_source as1, as2;

    fifo.push_back(1, as1);
    fifo.push_back(2, as1);
    fifo.push_back(3);
    fifo.push_back(4, as2);

    as1.request_abort();
    yield().get();

    BOOST_REQUIRE(!fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 2u);
    BOOST_REQUIRE(bool(fifo));
    BOOST_REQUIRE_EQUAL(expired.size(), 2u);
    std::sort(expired.begin(), expired.end());
    BOOST_REQUIRE_EQUAL(expired[0], 1);
    BOOST_REQUIRE_EQUAL(expired[1], 2);
    BOOST_REQUIRE_EQUAL(fifo.front(), 3);
    fifo.pop_front();
    BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
    BOOST_REQUIRE_EQUAL(fifo.front(), 4);
    fifo.pop_front();
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);

    expired.clear();

    as = abort_source();

    fifo.push_back(1);
    fifo.push_back(2, as);
    fifo.push_back(3, as);
    fifo.push_back(4, as);

    as.request_abort();
    yield().get();

    BOOST_REQUIRE(!fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
    BOOST_REQUIRE(bool(fifo));
    BOOST_REQUIRE_EQUAL(expired.size(), 3u);
    std::sort(expired.begin(), expired.end());
    BOOST_REQUIRE_EQUAL(expired[0], 2);
    BOOST_REQUIRE_EQUAL(expired[1], 3);
    BOOST_REQUIRE_EQUAL(expired[2], 4);
    BOOST_REQUIRE_EQUAL(fifo.front(), 1);
    fifo.pop_front();
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);

    expired.clear();
    as = abort_source();

    fifo.push_back(1);
    fifo.push_back(2, as);
    fifo.push_back(3, as);
    fifo.push_back(4, as);
    fifo.push_back(5);

    as.request_abort();
    yield().get();

    BOOST_REQUIRE_EQUAL(fifo.size(), 2u);
    BOOST_REQUIRE_EQUAL(fifo.front(), 1);
    fifo.pop_front();
    BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
    BOOST_REQUIRE_EQUAL(fifo.front(), 5);
    fifo.pop_front();
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
}

SEASTAR_THREAD_TEST_CASE(test_abort_exception) {
    struct entry {
        int value;
        std::exception_ptr ex;
    };
    std::vector<entry> expired;
    struct my_expiry {
        std::vector<entry>& e;
        void operator()(int& v, const std::optional<std::exception_ptr>& ex) noexcept { e.push_back(entry{v, ex.value_or(nullptr)}); }
    };

    internal::abortable_fifo<int, my_expiry> fifo(my_expiry{expired});
    auto aoe = abort_on_expiry<manual_clock>(manual_clock::now() + 1s);

    fifo.push_back(1, aoe.abort_source());

    BOOST_REQUIRE(!fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 1u);
    BOOST_REQUIRE(bool(fifo));
    BOOST_REQUIRE_EQUAL(fifo.front(), 1);

    manual_clock::advance(1s);
    yield().get();

    BOOST_REQUIRE(fifo.empty());
    BOOST_REQUIRE_EQUAL(fifo.size(), 0u);
    BOOST_REQUIRE(!bool(fifo));
    BOOST_REQUIRE_EQUAL(expired.size(), 1u);
    BOOST_REQUIRE_EQUAL(expired[0].value, 1);
    BOOST_REQUIRE(expired[0].ex);
    BOOST_REQUIRE_THROW(std::rethrow_exception(expired[0].ex), timed_out_error);
}
