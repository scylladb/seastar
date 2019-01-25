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
 * Copyright (C) 2017 ScyllaDB
 */

#define BOOST_TEST_MODULE core

#include <seastar/util/tuple_utils.hh>

#include <boost/test/included/unit_test.hpp>

#include <sstream>
#include <type_traits>

using namespace seastar;

BOOST_AUTO_TEST_CASE(map) {
    const auto pairs = tuple_map(std::make_tuple(10, 5.5, true), [](auto&& e) { return std::make_tuple(e, e); });

    BOOST_REQUIRE(pairs == std::make_tuple(std::make_tuple(10, 10),
                                           std::make_tuple(5.5, 5.5),
                                           std::make_tuple(true, true)));
}

BOOST_AUTO_TEST_CASE(for_each) {
    std::ostringstream os;

    tuple_for_each(std::make_tuple('a', 10, false, 5.4), [&os](auto&& e) {
        os << e;
    });

    BOOST_REQUIRE_EQUAL(os.str(), "a1005.4");
}

namespace {

template <typename T>
struct transform_type final {
    using type = T;
};

template <>
struct transform_type<bool> final { using type = int; };

template <>
struct transform_type<double> final { using type = char; };

}

BOOST_AUTO_TEST_CASE(map_types) {
    using before_tuple = std::tuple<double, bool, const char*>;
    using after_tuple = typename tuple_map_types<transform_type, before_tuple>::type;

    BOOST_REQUIRE((std::is_same<after_tuple, std::tuple<char, int, const char*>>::value));
}

namespace {

//
// Strip all `bool` fields.
//

template <typename>
struct keep_type final {
    static constexpr auto value = true;
};

template <>
struct keep_type<bool> final {
    static constexpr auto value = false;
};

}

BOOST_AUTO_TEST_CASE(filter_by_type) {
    using before_tuple = std::tuple<bool, int, bool, double, bool, char>;

    const auto t = tuple_filter_by_type<keep_type>(before_tuple{true, 10, false, 5.5, true, 'a'});
    using filtered_type = typename std::decay<decltype(t)>::type;

    BOOST_REQUIRE((std::is_same<filtered_type, std::tuple<int, double, char>>::value));
    BOOST_REQUIRE(t == std::make_tuple(10, 5.5, 'a'));
}
