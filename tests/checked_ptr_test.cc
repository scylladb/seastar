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
 * Copyright 2016 ScyllaDB
 */

#define BOOST_TEST_MODULE core

#include <boost/test/included/unit_test.hpp>
#include "core/checked_ptr.hh"
#include "core/weak_ptr.hh"

struct my_st : public weakly_referencable<my_st> {
        my_st(int a_) : a(a_) {}
        int a;
};

void const_ref_check_naked(const seastar::checked_ptr<my_st*>& cp) {
    BOOST_REQUIRE(bool(cp));
    BOOST_REQUIRE((*cp).a == 3);
    BOOST_REQUIRE(cp->a == 3);
    BOOST_REQUIRE(cp.get()->a == 3);
}

void const_ref_check_smart(const seastar::checked_ptr<::weak_ptr<my_st>>& cp) {
    BOOST_REQUIRE(bool(cp));
    BOOST_REQUIRE((*cp).a == 3);
    BOOST_REQUIRE(cp->a == 3);
    BOOST_REQUIRE(cp.get()->a == 3);
}

BOOST_AUTO_TEST_CASE(test_checked_ptr_is_empty_when_default_initialized) {
    seastar::checked_ptr<int*> cp;
    BOOST_REQUIRE(!bool(cp));
}

BOOST_AUTO_TEST_CASE(test_checked_ptr_is_empty_when_nullptr_initialized_nakes_ptr) {
    seastar::checked_ptr<int*> cp = nullptr;
    BOOST_REQUIRE(!bool(cp));
}

BOOST_AUTO_TEST_CASE(test_checked_ptr_is_empty_when_nullptr_initialized_smart_ptr) {
    seastar::checked_ptr<::weak_ptr<my_st>> cp = nullptr;
    BOOST_REQUIRE(!bool(cp));
}

BOOST_AUTO_TEST_CASE(test_checked_ptr_is_initialized_after_assignment_naked_ptr) {
    seastar::checked_ptr<my_st*> cp = nullptr;
    BOOST_REQUIRE(!bool(cp));
    my_st i(3);
    my_st k(3);
    cp = &i;
    seastar::checked_ptr<my_st*> cp1(&i);
    seastar::checked_ptr<my_st*> cp2(&k);
    BOOST_REQUIRE(bool(cp));
    BOOST_REQUIRE(cp == cp1);
    BOOST_REQUIRE(cp != cp2);
    BOOST_REQUIRE((*cp).a == 3);
    BOOST_REQUIRE(cp->a == 3);
    BOOST_REQUIRE(cp.get()->a == 3);

    const_ref_check_naked(cp);

    cp = nullptr;
    BOOST_REQUIRE(!bool(cp));
}

BOOST_AUTO_TEST_CASE(test_checked_ptr_is_initialized_after_assignment_smart_ptr) {
    seastar::checked_ptr<::weak_ptr<my_st>> cp = nullptr;
    BOOST_REQUIRE(!bool(cp));
    std::unique_ptr<my_st> i = std::make_unique<my_st>(3);
    cp = i->weak_from_this();
    seastar::checked_ptr<::weak_ptr<my_st>> cp1(i->weak_from_this());
    seastar::checked_ptr<::weak_ptr<my_st>> cp2;
    BOOST_REQUIRE(bool(cp));
    BOOST_REQUIRE(cp == cp1);
    BOOST_REQUIRE(cp != cp2);
    BOOST_REQUIRE((*cp).a == 3);
    BOOST_REQUIRE(cp->a == 3);
    BOOST_REQUIRE(cp.get()->a == 3);

    const_ref_check_smart(cp);

    i = nullptr;
    BOOST_REQUIRE(!bool(cp));
    BOOST_REQUIRE(!bool(cp1));
    BOOST_REQUIRE(!bool(cp2));
}

