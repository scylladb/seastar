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
 * Copyright (C) 2018 ScyllaDB Ltd.
 */

#include <seastar/testing/entry_point.hh>
#include <seastar/testing/seastar_test.hh>
#include <seastar/testing/test_runner.hh>

namespace seastar {

namespace testing {

static bool init_unit_test_suite() {
    const auto& tests = known_tests();
    auto&& ts = boost::unit_test::framework::master_test_suite();
    ts.p_name.set(tests.size() ? (tests)[0]->get_test_file() : "seastar-tests");

    for (seastar_test* test : tests) {
#if BOOST_VERSION > 105800
        ts.add(boost::unit_test::make_test_case([test] { test->run(); }, test->get_name(),
                                                test->get_test_file(), 0), 0, 0);
#else
        ts.add(boost::unit_test::make_test_case([test] { test->run(); }, test->get_name()), 0, 0);
#endif
    }

    return global_test_runner().start(ts.argc, ts.argv);
}

int entry_point(int argc, char** argv) {
    const int boost_exit_code = ::boost::unit_test::unit_test_main(&init_unit_test_suite, argc, argv);
    const int seastar_exit_code = seastar::testing::global_test_runner().finalize();
    if (boost_exit_code) {
        return boost_exit_code;
    }
    return seastar_exit_code;
}

}

}