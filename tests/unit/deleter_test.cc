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
 * Copyright (C) 2019 Lightbits Labs Ltd. - All Rights Reserved
*/

#define BOOST_TEST_MODULE core

#include <boost/test/unit_test.hpp>
#include <seastar/core/deleter.hh>
#include <seastar/util/alloc_failure_injector.hh>
#include <seastar/util/defer.hh>

using namespace seastar;

struct TestObject {
      TestObject() : has_ref(true){}
      TestObject(TestObject&& other) {
          has_ref = true;
          other.has_ref = false;
      }
      ~TestObject() {
          if (has_ref) {
              ++deletions_called;
          }
      }
      static int deletions_called;
      int has_ref;
};
int TestObject::deletions_called = 0;

BOOST_AUTO_TEST_CASE(test_deleter_append_does_not_free_shared_object) {
    {
        deleter tested;
        {
            auto obj1 = TestObject();
            deleter del1 = make_object_deleter(std::move(obj1));
            auto obj2 = TestObject();
            deleter del2 = make_object_deleter(std::move(obj2));
            del1.append(std::move(del2));
            tested = del1.share();
            auto obj3 = TestObject();
            deleter del3 = make_object_deleter(std::move(obj3));
            del1.append(std::move(del3));
        }
        // since deleter tested still holds references to first two objects, last objec should be deleted
        BOOST_REQUIRE(TestObject::deletions_called == 1);
    }
    BOOST_REQUIRE(TestObject::deletions_called == 3);
}

BOOST_AUTO_TEST_CASE(test_deleter_append_same_shared_object_twice) {
    TestObject::deletions_called = 0;
    {
        deleter tested;
        {
            deleter del1 = make_object_deleter(TestObject());
            auto del2 = del1.share();

            tested.append(std::move(del1));
            tested.append(std::move(del2));
        }
        BOOST_REQUIRE(TestObject::deletions_called == 0);
    }
    BOOST_REQUIRE(TestObject::deletions_called == 1);
}

#ifdef SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION
BOOST_AUTO_TEST_CASE(test_deleter_append_to_shared_failed_allocation_keeps_refs) {
    int freed = 0;
    deleter del1 = make_deleter([&freed] { ++freed; });
    deleter del2 = del1.share();
    deleter del3 = make_deleter([] {});

    {
        auto& injector = memory::local_failure_injector();
        auto cancel = defer([&injector] () noexcept { injector.cancel(); });
        injector.fail_after(0);
        BOOST_REQUIRE_THROW(del1.append(std::move(del3)), std::bad_alloc);
        BOOST_REQUIRE(injector.failed());
    }

    // Both del1 and del2 still hold a reference to the shared impl.
    del1 = deleter();
    BOOST_REQUIRE_EQUAL(freed, 0);
    del2 = deleter();
    BOOST_REQUIRE_EQUAL(freed, 1);
}
#endif
