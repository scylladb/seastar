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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#pragma once

#include <exception>
#include <string_view>
#include <boost/test/unit_test.hpp> // IWYU pragma: export
#include <seastar/core/future.hh>
#include <seastar/testing/entry_point.hh> // IWYU pragma: keep

#define SEASTAR_TEST_INVOKE(func, ...) func(__VA_ARGS__)

namespace seastar {

namespace testing {

class seastar_test {
    const std::string _test_file;
public:
    seastar_test(const char* test_name, const char* test_file, int test_line);
    seastar_test(const char* test_name, const char* test_file, int test_line,
                 boost::unit_test::decorator::collector_t& decorators);
    virtual ~seastar_test() {}
    const std::string& get_test_file() const {
        return _test_file;
    }
    static const std::string& get_name();
    virtual future<> run_test_case() const = 0;
    void run();
};

// BOOST_REQUIRE_EXCEPTION predicates
namespace exception_predicate {

std::function<bool(const std::exception&)> message_equals(std::string_view expected_message);
std::function<bool(const std::exception&)> message_contains(std::string_view expected_message);

} // exception_predicate

}

}

#ifdef SEASTAR_TESTING_MAIN

int main(int argc, char** argv) {
    return seastar::testing::entry_point(argc, argv);
}

#endif // SEASTAR_TESTING_MAIN
