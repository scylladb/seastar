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

#include <boost/test/unit_test.hpp>
#include <seastar/core/weak_ptr.hh>

using namespace seastar;

class myclass : public weakly_referencable<myclass> {};

static_assert(std::is_nothrow_default_constructible_v<myclass>);

static_assert(std::is_nothrow_default_constructible_v<weak_ptr<myclass>>);
static_assert(std::is_nothrow_move_constructible_v<weak_ptr<myclass>>);

BOOST_AUTO_TEST_CASE(test_weak_ptr_is_empty_when_default_initialized) {
    weak_ptr<myclass> wp;
    BOOST_REQUIRE(!bool(wp));
}

BOOST_AUTO_TEST_CASE(test_weak_ptr_is_reset) {
    auto owning_ptr = std::make_unique<myclass>();
    weak_ptr<myclass> wp = owning_ptr->weak_from_this();
    BOOST_REQUIRE(bool(wp));
    BOOST_REQUIRE(&*wp == &*owning_ptr);
    owning_ptr = {};
    BOOST_REQUIRE(!bool(wp));
}

BOOST_AUTO_TEST_CASE(test_weak_ptr_can_be_moved) {
    auto owning_ptr = std::make_unique<myclass>();
    weak_ptr<myclass> wp1 = owning_ptr->weak_from_this();
    weak_ptr<myclass> wp2 = owning_ptr->weak_from_this();
    weak_ptr<myclass> wp3 = owning_ptr->weak_from_this();

    weak_ptr<myclass> wp3_moved;
    wp3_moved = std::move(wp3);
    weak_ptr<myclass> wp1_moved(std::move(wp1));
    auto wp2_moved = std::move(wp2);
    BOOST_REQUIRE(!bool(wp1));
    BOOST_REQUIRE(!bool(wp2));
    BOOST_REQUIRE(!bool(wp3));
    BOOST_REQUIRE(bool(wp1_moved));
    BOOST_REQUIRE(bool(wp2_moved));
    BOOST_REQUIRE(bool(wp3_moved));
    BOOST_REQUIRE(wp1_moved.get() == owning_ptr.get());
    BOOST_REQUIRE(wp2_moved.get() == owning_ptr.get());
    BOOST_REQUIRE(wp3_moved.get() == owning_ptr.get());

    owning_ptr = {};

    BOOST_REQUIRE(!bool(wp1_moved));
    BOOST_REQUIRE(!bool(wp2_moved));
    BOOST_REQUIRE(!bool(wp3_moved));
}

BOOST_AUTO_TEST_CASE(test_weak_ptr_can_be_copied) {
    auto owning_ptr = std::make_unique<myclass>();
    weak_ptr<myclass> wp1 = owning_ptr->weak_from_this();
    weak_ptr<myclass> wp2 = owning_ptr->weak_from_this();
    weak_ptr<myclass> wp3 = owning_ptr->weak_from_this();

    weak_ptr<myclass> wp1_copied(wp1);
    auto wp2_copied = wp2;
    weak_ptr<myclass> wp3_copied;
    wp3_copied = wp3;
    BOOST_REQUIRE(bool(wp1));
    BOOST_REQUIRE(bool(wp2));
    BOOST_REQUIRE(bool(wp3));
    BOOST_REQUIRE(bool(wp1_copied));
    BOOST_REQUIRE(bool(wp2_copied));
    BOOST_REQUIRE(bool(wp3_copied));

    BOOST_REQUIRE(wp1.get() == wp1_copied.get());
    BOOST_REQUIRE(wp2.get() == wp2_copied.get());
    BOOST_REQUIRE(wp3.get() == wp3_copied.get());

    owning_ptr = {};

    BOOST_REQUIRE(!bool(wp1));
    BOOST_REQUIRE(!bool(wp2));
    BOOST_REQUIRE(!bool(wp3));
    BOOST_REQUIRE(!bool(wp1_copied));
    BOOST_REQUIRE(!bool(wp2_copied));
    BOOST_REQUIRE(!bool(wp3_copied));
}

BOOST_AUTO_TEST_CASE(test_multipe_weak_ptrs) {
    auto owning_ptr = std::make_unique<myclass>();

    weak_ptr<myclass> wp1 = owning_ptr->weak_from_this();
    BOOST_REQUIRE(bool(wp1));
    BOOST_REQUIRE(&*wp1 == &*owning_ptr);

    weak_ptr<myclass> wp2 = owning_ptr->weak_from_this();
    BOOST_REQUIRE(bool(wp2));
    BOOST_REQUIRE(&*wp2 == &*owning_ptr);

    owning_ptr = {};

    BOOST_REQUIRE(!bool(wp1));
    BOOST_REQUIRE(!bool(wp2));
}

BOOST_AUTO_TEST_CASE(test_multipe_weak_ptrs_going_away_first) {
    auto owning_ptr = std::make_unique<myclass>();

    weak_ptr<myclass> wp1 = owning_ptr->weak_from_this();
    weak_ptr<myclass> wp2 = owning_ptr->weak_from_this();
    weak_ptr<myclass> wp3 = owning_ptr->weak_from_this();

    BOOST_REQUIRE(bool(wp1));
    BOOST_REQUIRE(bool(wp2));
    BOOST_REQUIRE(bool(wp3));

    wp2 = {};

    owning_ptr = std::make_unique<myclass>();

    BOOST_REQUIRE(!bool(wp1));
    BOOST_REQUIRE(!bool(wp2));
    BOOST_REQUIRE(!bool(wp3));

    wp1 = owning_ptr->weak_from_this();
    wp2 = owning_ptr->weak_from_this();
    wp3 = owning_ptr->weak_from_this();

    BOOST_REQUIRE(bool(wp1));
    BOOST_REQUIRE(bool(wp2));
    BOOST_REQUIRE(bool(wp3));

    wp3 = {};
    owning_ptr = std::make_unique<myclass>();

    BOOST_REQUIRE(!bool(wp1));
    BOOST_REQUIRE(!bool(wp2));
    BOOST_REQUIRE(!bool(wp3));

    wp1 = owning_ptr->weak_from_this();
    wp2 = owning_ptr->weak_from_this();
    wp3 = owning_ptr->weak_from_this();

    wp1 = {};
    wp3 = {};
    owning_ptr = std::make_unique<myclass>();

    BOOST_REQUIRE(!bool(wp1));
    BOOST_REQUIRE(!bool(wp2));
    BOOST_REQUIRE(!bool(wp3));
}
