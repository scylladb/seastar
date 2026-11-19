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
 * Copyright (C) 2026 ScyllaDB
 */

// Tests of lw_shared_ptr<T> where T is incomplete: this file sees only the
// declarations in lw_shared_ptr_incomplete.hh. The definitions are in
// lw_shared_ptr_incomplete_types.cc, which must be a separate file:
// compilers instantiate templates at the end of a file, where T would
// already be complete.

#define BOOST_TEST_MODULE core

#include <boost/test/unit_test.hpp>
#include <vector>

#include "lw_shared_ptr_incomplete.hh"

using namespace seastar;
using namespace incomplete;

BOOST_AUTO_TEST_CASE(test_incomplete_without_esft) {
    int before = destroyed;
    {
        auto p = make_plain(1);
        auto q = p;
        lw_shared_ptr<const plain> c = p;
        std::vector<lw_shared_ptr<plain>> v{p, q};
        v.push_back(std::move(p));
        BOOST_REQUIRE(!p);
        BOOST_REQUIRE_EQUAL(q.use_count(), 5);
        BOOST_REQUIRE(v[0] == q && c == q);
        v.clear();
        q = nullptr;
        BOOST_REQUIRE_EQUAL(value(c), 1);
        BOOST_REQUIRE_EQUAL(destroyed, before);
    }
    // The last pointer was an lw_shared_ptr<const plain>.
    BOOST_REQUIRE_EQUAL(destroyed, before + 1);
}

BOOST_AUTO_TEST_CASE(test_incomplete_with_esft) {
    int before = destroyed;
    {
        auto p = make_with_esft(2);
        auto q = shared_from_this(p);
        BOOST_REQUIRE(p == q);
        BOOST_REQUIRE_EQUAL(p.use_count(), 2);
        p = std::move(q);
        BOOST_REQUIRE_EQUAL(value(p), 2);
        BOOST_REQUIRE_EQUAL(destroyed, before);
    }
    BOOST_REQUIRE_EQUAL(destroyed, before + 1);
}

BOOST_AUTO_TEST_CASE(test_incomplete_dispose_hook) {
    int before = destroyed;
    int before_hooked = hooked_disposals;
    {
        auto p = make_hooked();
        lw_shared_ptr<const hooked> q = p;
    }
    BOOST_REQUIRE_EQUAL(hooked_disposals, before_hooked + 1);
    BOOST_REQUIRE_EQUAL(destroyed, before + 1);
}

BOOST_AUTO_TEST_CASE(test_incomplete_deferred_dispose) {
    int before = destroyed;
    {
        auto p = make_deferred();
        auto q = p;
    }
    BOOST_REQUIRE_EQUAL(destroyed, before);
    dispose_deferred();
    BOOST_REQUIRE_EQUAL(destroyed, before + 1);
}

// The old way (see lw_shared_ptr_incomplete.hh).
BOOST_AUTO_TEST_CASE(test_incomplete_old_way) {
    int before = destroyed;
    {
        auto p = make_old_way();
        auto q = p;
        lw_shared_ptr<const old_way> c = q;
        BOOST_REQUIRE_EQUAL(p.use_count(), 3);
    }
    BOOST_REQUIRE_EQUAL(old_way_disposals, 1);
    BOOST_REQUIRE_EQUAL(destroyed, before + 1);
}

namespace complete {

// The hook also works for a complete type, without an explicit instantiation.
struct hooked {
    static inline int disposals = 0;
};

void lw_shared_ptr_dispose(hooked* p) noexcept {
    hooked::disposals++;
    lw_shared_ptr<hooked>::dispose(p);
}

}

BOOST_AUTO_TEST_CASE(test_complete_dispose_hook) {
    {
        auto p = make_lw_shared<complete::hooked>();
        auto q = p;
    }
    BOOST_REQUIRE_EQUAL(complete::hooked::disposals, 1);
}

namespace hierarchy {

// A disposal function for a base class isn't used for a derived class.
struct base {
    static inline int disposals = 0;
};

void lw_shared_ptr_dispose(base* p) noexcept {
    base::disposals++;
    lw_shared_ptr<base>::dispose(p);
}

struct derived : base {
    static inline int destroyed = 0;
    ~derived() { destroyed++; }
};

// Another class, with a disposal function in the same namespace, mustn't
// change which function is used for the others.
struct other {
    static inline int disposals = 0;
};

void lw_shared_ptr_dispose(other* p) noexcept {
    other::disposals++;
    lw_shared_ptr<other>::dispose(p);
}

}

BOOST_AUTO_TEST_CASE(test_dispose_hook_not_inherited) {
    {
        auto p = make_lw_shared<hierarchy::derived>();
    }
    BOOST_REQUIRE_EQUAL(hierarchy::base::disposals, 0);
    BOOST_REQUIRE_EQUAL(hierarchy::derived::destroyed, 1);
    {
        auto p = make_lw_shared<hierarchy::base>();
    }
    BOOST_REQUIRE_EQUAL(hierarchy::base::disposals, 1);
    {
        auto p = make_lw_shared<hierarchy::other>();
    }
    BOOST_REQUIRE_EQUAL(hierarchy::other::disposals, 1);
    BOOST_REQUIRE_EQUAL(hierarchy::base::disposals, 1);
}
