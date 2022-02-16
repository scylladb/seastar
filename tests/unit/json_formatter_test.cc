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
 * Copyright (C) 2016 ScyllaDB.
 */
#include <vector>

#include <seastar/core/do_with.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/do_with.hh>
#include <seastar/json/formatter.hh>

using namespace seastar;
using namespace json;

SEASTAR_TEST_CASE(test_simple_values) {
    BOOST_CHECK_EQUAL("3", formatter::to_json(3));
    BOOST_CHECK_EQUAL("3", formatter::to_json(3.0));
    BOOST_CHECK_EQUAL("3.5", formatter::to_json(3.5));
    BOOST_CHECK_EQUAL("true", formatter::to_json(true));
    BOOST_CHECK_EQUAL("false", formatter::to_json(false));

    BOOST_CHECK_EQUAL("\"apa\"", formatter::to_json("apa")); // to_json(const char*)
    BOOST_CHECK_EQUAL("\"apa\"", formatter::to_json(sstring("apa"))); // to_json(const sstring&)
    BOOST_CHECK_EQUAL("\"apa\"", formatter::to_json("apa", 3)); // to_json(const char*, size_t)

    using namespace std::string_literals;
    sstring str = "\0 COWA\bU\nGA [{\r}]\x1a"s,
            expected = "\"\\u0000 COWA\\bU\\nGA [{\\r}]\\u001A\""s;
    BOOST_CHECK_EQUAL(expected, formatter::to_json(str)); // to_json(const sstring&)
    BOOST_CHECK_EQUAL(expected, formatter::to_json(str.c_str(), str.size())); // to_json(const char*, size_t)

    return make_ready_future();
}

SEASTAR_TEST_CASE(test_collections) {
    BOOST_CHECK_EQUAL("{1:2,3:4}", formatter::to_json(std::map<int,int>({{1,2},{3,4}})));
    BOOST_CHECK_EQUAL("[1,2,3,4]", formatter::to_json(std::vector<int>({1,2,3,4})));
    BOOST_CHECK_EQUAL("[{1:2},{3:4}]", formatter::to_json(std::vector<std::pair<int,int>>({{1,2},{3,4}})));
    BOOST_CHECK_EQUAL("[{1:2},{3:4}]", formatter::to_json(std::vector<std::map<int,int>>({{{1,2}},{{3,4}}})));
    BOOST_CHECK_EQUAL("[[1,2],[3,4]]", formatter::to_json(std::vector<std::vector<int>>({{1,2},{3,4}})));

    return make_ready_future();
}
