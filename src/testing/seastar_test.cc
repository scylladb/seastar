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

#include <thread>
#include <iostream>

#include <seastar/testing/entry_point.hh>
#include <seastar/testing/seastar_test.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/core/future.hh>
#include <seastar/core/on_internal_error.hh>
#include <seastar/core/app-template.hh>
#include <seastar/testing/on_internal_error.hh>

namespace seastar {

namespace testing {

exchanger_base::exchanger_base() { }
exchanger_base::~exchanger_base() { }

void seastar_test::run() {
    // HACK: please see https://github.com/cloudius-systems/seastar/issues/10
    BOOST_REQUIRE(true);

    // HACK: please see https://github.com/cloudius-systems/seastar/issues/10
    boost::program_options::variables_map()["dummy"];

    set_abort_on_internal_error(true);

    global_test_runner().run_sync([this] {
        return run_test_case();
    });
}

seastar_test::seastar_test(const char* test_name, const char* test_file, int test_line)
    : seastar_test(test_name, test_file, test_line, boost::unit_test::decorator::collector_t::instance()) {}

seastar_test::seastar_test(const char* test_name, const char* test_file, int test_line,
                           boost::unit_test::decorator::collector_t& decorators)
    : _test_file{test_file} {
    auto test = boost::unit_test::make_test_case([this] { run(); }, test_name, test_file, test_line);
    decorators.store_in(*test);
    decorators.reset();
    boost::unit_test::framework::current_auto_test_suite().add(test);
}

const std::string& seastar_test::get_name() {
    const auto& current_test = boost::unit_test::framework::current_test_unit();
    return current_test.p_name.get();
}

namespace exception_predicate {

std::function<bool(const std::exception&)> message_equals(std::string_view expected_message) {
    return [expected_message] (const std::exception& e) {
        std::string error = e.what();
        if (error == expected_message) {
            return true;
        } else {
            std::cerr << "Expected \"" << expected_message << "\" but got \"" << error << '"' << std::endl;
            return false;
        }
    };
}

std::function<bool(const std::exception&)> message_contains(std::string_view expected_message) {
    return [expected_message] (const std::exception& e) {
        std::string error = e.what();
        if (error.find(expected_message.data()) != std::string::npos) {
            return true;
        } else {
            std::cerr << "Expected \"" << expected_message << "\" but got \"" << error << '"' << std::endl;
            return false;
        }
    };
}

} // exception_predicate

scoped_no_abort_on_internal_error::scoped_no_abort_on_internal_error() noexcept
    : _prev(set_abort_on_internal_error(false))
{
}

scoped_no_abort_on_internal_error::~scoped_no_abort_on_internal_error() {
    set_abort_on_internal_error(_prev);
}

}

}
