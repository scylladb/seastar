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
 * Copyright 2023 ScyllaDB
 */
#define BOOST_TEST_MODULE util

#include <boost/test/unit_test.hpp>
#include <seastar/core/units.hh>
#include <seastar/util/conversions.hh>

using namespace seastar;

BOOST_AUTO_TEST_CASE(format_iec) {
    struct {
        size_t n;
        std::string_view formatted;
    } sizes[] = {
       {0, "0"},
       {42, "42"},
       {10_KiB, "10Ki"},
       {10_MiB, "10Mi"},
       {10_GiB, "10Gi"},
       {10_TiB, "10Ti"},
       {10'000_TiB, "10000Ti"},
    };
    for (auto [n, expected] : sizes) {
        std::string actual;
        fmt::format_to(std::back_inserter(actual), "{:i}", data_size{n});
        BOOST_CHECK_EQUAL(actual, expected);
    }
}

BOOST_AUTO_TEST_CASE(format_si) {
    struct {
        size_t n;
        std::string_view formatted;
    } sizes[] = {
       {0ULL, "0"},
       {42ULL, "42"},
       {10'000ULL, "10k"},
       {10'000'000ULL, "10M"},
       {10'000'000'000ULL, "10G"},
       {10'000'000'000'000ULL, "10T"},
       {10'000'000'000'000'000ULL, "10000T"},
    };
    for (auto [n, expected] : sizes) {
        std::string actual;
        fmt::format_to(std::back_inserter(actual), "{:s}", data_size{n});
        BOOST_CHECK_EQUAL(actual, expected);
    }
}
