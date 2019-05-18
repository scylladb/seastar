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
 * Copyright (C) 2019 ScyllaDB
 */

#define BOOST_TEST_MODULE core

#include <boost/test/included/unit_test.hpp>
#include "../../src/core/uname.hh"

using namespace seastar::internal;

BOOST_AUTO_TEST_CASE(test_nowait_aio_fix) {
    auto check = [] (const char* uname) {
        return parse_uname(uname).whitelisted({"5.1", "5.0.8", "4.19.35", "4.14.112"});
    };
    BOOST_REQUIRE_EQUAL(check("5.1.0"), true);
    BOOST_REQUIRE_EQUAL(check("5.1.1"), true);
    BOOST_REQUIRE_EQUAL(check("5.1.1-44.distro"), true);
    BOOST_REQUIRE_EQUAL(check("5.1.1-44.7.distro"), true);
    BOOST_REQUIRE_EQUAL(check("5.0.0"), false);
    BOOST_REQUIRE_EQUAL(check("5.0.7"), false);
    BOOST_REQUIRE_EQUAL(check("5.0.7-55.el19"), false);
    BOOST_REQUIRE_EQUAL(check("5.0.8"), true);
    BOOST_REQUIRE_EQUAL(check("5.0.9"), true);
    BOOST_REQUIRE_EQUAL(check("5.0.8-200.fedora"), true);
    BOOST_REQUIRE_EQUAL(check("5.0.9-200.fedora"), true);
    BOOST_REQUIRE_EQUAL(check("5.2.0"), true);
    BOOST_REQUIRE_EQUAL(check("5.2.9"), true);
    BOOST_REQUIRE_EQUAL(check("5.2.9-77.el153"), true);
    BOOST_REQUIRE_EQUAL(check("6.0.0"), true);
    BOOST_REQUIRE_EQUAL(check("3.9.0"), false);
    BOOST_REQUIRE_EQUAL(check("4.19"), false);
    BOOST_REQUIRE_EQUAL(check("4.19.34"), false);
    BOOST_REQUIRE_EQUAL(check("4.19.35"), true);
    BOOST_REQUIRE_EQUAL(check("4.19.36"), true);
    BOOST_REQUIRE_EQUAL(check("4.20.36"), false);
    BOOST_REQUIRE_EQUAL(check("4.14.111"), false);
    BOOST_REQUIRE_EQUAL(check("4.14.112"), true);
    BOOST_REQUIRE_EQUAL(check("4.14.113"), true);
}


BOOST_AUTO_TEST_CASE(test_xfs_concurrency_fix) {
    auto check = [] (const char* uname) {
        return parse_uname(uname).whitelisted({"3.15", "3.10.0-325.el7"});
    };
    BOOST_REQUIRE_EQUAL(check("3.15.0"), true);
    BOOST_REQUIRE_EQUAL(check("5.1.0"), true);
    BOOST_REQUIRE_EQUAL(check("3.14.0"), false);
    BOOST_REQUIRE_EQUAL(check("3.10.0"), false);
    BOOST_REQUIRE_EQUAL(check("3.10.14"), false);
    BOOST_REQUIRE_EQUAL(check("3.10.0-325.ubuntu"), false);
    BOOST_REQUIRE_EQUAL(check("3.10.0-325"), false);
    BOOST_REQUIRE_EQUAL(check("3.10.0-325.el7"), true);
    BOOST_REQUIRE_EQUAL(check("3.10.0-326.el7"), true);
    BOOST_REQUIRE_EQUAL(check("3.10.0-324.el7"), false);
    BOOST_REQUIRE_EQUAL(check("3.10.0-325.665.el7"), true);
}
