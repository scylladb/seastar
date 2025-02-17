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
 * Copyright 2015 Cloudius Systems
 */

#define BOOST_TEST_MODULE core

#include <boost/test/unit_test.hpp>
#include <set>
#include <unordered_map>
#include <seastar/core/sstring.hh>
#include <seastar/core/shared_ptr.hh>

using namespace seastar;

struct expected_exception : public std::exception {};

struct A {
    static bool destroyed;
    A() {
        destroyed = false;
    }
    virtual ~A() {
        destroyed = true;
    }
};

struct A_esft : public A, public enable_lw_shared_from_this<A_esft> {
};

struct B {
    virtual void x() {}
};

bool A::destroyed = false;

BOOST_AUTO_TEST_CASE(explot_dynamic_cast_use_after_free_problem) {
    shared_ptr<A> p = ::make_shared<A>();
    {
        auto p2 = dynamic_pointer_cast<B>(p);
        SEASTAR_ASSERT(!p2);
    }
    SEASTAR_ASSERT(!A::destroyed);
}

class C : public enable_shared_from_this<C> {
public:
    shared_ptr<C> dup() { return shared_from_this(); }
    shared_ptr<const C> get() const { return shared_from_this(); }
};

BOOST_AUTO_TEST_CASE(test_const_ptr) {
    shared_ptr<C> a = make_shared<C>();
    shared_ptr<const C> ca = a;
    BOOST_REQUIRE(ca == a);
    shared_ptr<const C> cca = ca->get();
    BOOST_REQUIRE(cca == ca);
}

struct D {};

BOOST_AUTO_TEST_CASE(test_lw_const_ptr_1) {
    auto pd1 = make_lw_shared<const D>(D());
    auto pd2 = make_lw_shared(D());
    lw_shared_ptr<const D> pd3 = pd2;
    BOOST_REQUIRE(pd2 == pd3);
}

struct E : enable_lw_shared_from_this<E> {};

BOOST_AUTO_TEST_CASE(test_lw_const_ptr_2) {
    auto pe1 = make_lw_shared<const E>();
    auto pe2 = make_lw_shared<E>();
    lw_shared_ptr<const E> pe3 = pe2;
    BOOST_REQUIRE(pe2 == pe3);
}

struct F : enable_lw_shared_from_this<F> {
    auto const_method() const {
        return shared_from_this();
    }
};

BOOST_AUTO_TEST_CASE(test_shared_from_this_called_on_const_object) {
    auto ptr = make_lw_shared<F>();
    ptr->const_method();
}

BOOST_AUTO_TEST_CASE(test_exception_thrown_from_constructor_is_propagated) {
    struct X {
        X() {
            throw expected_exception();
        }
    };
    try {
        auto ptr = make_lw_shared<X>();
        BOOST_FAIL("Constructor should have thrown");
    } catch (const expected_exception& e) {
        BOOST_TEST_MESSAGE("Expected exception caught");
    }
    try {
        auto ptr = ::make_shared<X>();
        BOOST_FAIL("Constructor should have thrown");
    } catch (const expected_exception& e) {
        BOOST_TEST_MESSAGE("Expected exception caught");
    }
}

BOOST_AUTO_TEST_CASE(test_indirect_functors) {
    {
        std::multiset<shared_ptr<sstring>, indirect_less<shared_ptr<sstring>>> a_set;

        a_set.insert(make_shared<sstring>("k3"));
        a_set.insert(make_shared<sstring>("k1"));
        a_set.insert(make_shared<sstring>("k2"));
        a_set.insert(make_shared<sstring>("k4"));
        a_set.insert(make_shared<sstring>("k0"));


        auto i = a_set.begin();
        BOOST_REQUIRE_EQUAL(sstring("k0"), *(*i++));
        BOOST_REQUIRE_EQUAL(sstring("k1"), *(*i++));
        BOOST_REQUIRE_EQUAL(sstring("k2"), *(*i++));
        BOOST_REQUIRE_EQUAL(sstring("k3"), *(*i++));
        BOOST_REQUIRE_EQUAL(sstring("k4"), *(*i++));
    }

    {
        std::unordered_map<shared_ptr<sstring>, bool,
                indirect_hash<shared_ptr<sstring>>, indirect_equal_to<shared_ptr<sstring>>> a_map;

        a_map.emplace(make_shared<sstring>("k3"), true);
        a_map.emplace(make_shared<sstring>("k1"), true);
        a_map.emplace(make_shared<sstring>("k2"), true);
        a_map.emplace(make_shared<sstring>("k4"), true);
        a_map.emplace(make_shared<sstring>("k0"), true);

        BOOST_REQUIRE(a_map.count(make_shared<sstring>("k0")));
        BOOST_REQUIRE(a_map.count(make_shared<sstring>("k1")));
        BOOST_REQUIRE(a_map.count(make_shared<sstring>("k2")));
        BOOST_REQUIRE(a_map.count(make_shared<sstring>("k3")));
        BOOST_REQUIRE(a_map.count(make_shared<sstring>("k4")));
        BOOST_REQUIRE(!a_map.count(make_shared<sstring>("k5")));
    }
}

template<typename T>
void do_test_release() {
    auto ptr = make_lw_shared<T>();
    BOOST_REQUIRE(!T::destroyed);

    auto ptr2 = ptr;

    BOOST_REQUIRE(!ptr.release());
    BOOST_REQUIRE(!ptr);
    BOOST_REQUIRE(ptr2.use_count() == 1);

    auto uptr2 = ptr2.release();
    BOOST_REQUIRE(uptr2);
    BOOST_REQUIRE(!ptr2);
    ptr2 = {};

    BOOST_REQUIRE(!T::destroyed);
    uptr2 = {};

    BOOST_REQUIRE(T::destroyed);

    // Check destroying via disposer
    auto ptr3 = make_lw_shared<T>();
    auto uptr3 = ptr3.release();
    BOOST_REQUIRE(uptr3);
    BOOST_REQUIRE(!T::destroyed);

    auto raw_ptr3 = uptr3.release();
    lw_shared_ptr<T>::dispose(raw_ptr3);
    BOOST_REQUIRE(T::destroyed);
}

BOOST_AUTO_TEST_CASE(test_release) {
    do_test_release<A>();
    do_test_release<A_esft>();
}

BOOST_AUTO_TEST_CASE(test_const_release) {
    do_test_release<const A>();
    do_test_release<const A_esft>();
}

BOOST_AUTO_TEST_CASE(test_nullptr_compare) {
    seastar::shared_ptr<int> ptr;
    BOOST_REQUIRE(ptr == nullptr);
    BOOST_REQUIRE(nullptr == ptr);
    ptr = seastar::make_shared<int>(0);
    BOOST_REQUIRE(ptr != nullptr);
    BOOST_REQUIRE(nullptr != ptr);

    seastar::lw_shared_ptr<int> lptr;
    BOOST_REQUIRE(lptr == nullptr);
    BOOST_REQUIRE(nullptr == lptr);
    lptr = seastar::make_lw_shared<int>(0);
    BOOST_REQUIRE(lptr != nullptr);
    BOOST_REQUIRE(nullptr != lptr);
}
