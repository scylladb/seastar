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
 * Copyright (C) 2021 ScyllaDB
 */

#define BOOST_TEST_MODULE core

#include <boost/test/unit_test.hpp>

#include <seastar/util/std-compat.hh>

using namespace seastar;

static void test_source_location_callee(const char* ref_file, const char* ref_func, int ref_line, compat::source_location loc = compat::source_location::current()) {
    BOOST_REQUIRE_EQUAL(loc.file_name(), ref_file);
    BOOST_REQUIRE_EQUAL(loc.line(), ref_line);
    BOOST_REQUIRE(std::string(loc.function_name()).find(ref_func) != std::string::npos);
}

static void test_source_location_caller() {
    test_source_location_callee(__FILE__, __func__, __LINE__);
}

BOOST_AUTO_TEST_CASE(test_source_location) {
    test_source_location_caller();
}
