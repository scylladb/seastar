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

// Regression test for a GCC -Wmaybe-uninitialized false positive raised
// in seastar::future_state<T>::clear(). future_state<T> stores its
// payload in a discriminated raw-storage union; the payload is
// constructed in move_it() and destroyed in clear() under the same
// _u.has_result() predicate, so the invariant "value is constructed
// iff _u.has_result()" holds at runtime. GCC's data-flow analysis
// cannot correlate the two checks across function boundaries and, after
// -O2 inlining of ~future -> ~future_state -> clear() -> destroy_at(),
// flags the destroy as reading uninitialized memory whenever ~T() reads
// a member before destroying (e.g. ~foreign_ptr reads _cpu).
//
// The minimal trigger we know of is future<foreign_ptr<unique_ptr<T>>>.
// std::expected payloads and other shapes can amplify the warning to
// additional fields but are not required to reproduce it.
//
// The CMakeLists.txt entry for this test re-enables
// -Wmaybe-uninitialized for just this translation unit (it is
// otherwise disabled globally as a PUBLIC compile option on the
// seastar target) so any regression of the warning fails the build.

#include <memory>

#include <seastar/core/future.hh>
#include <seastar/core/sharded.hh>  // for seastar::foreign_ptr
#include <seastar/testing/test_case.hh>

using namespace seastar;

namespace {

using value_t = foreign_ptr<std::unique_ptr<int>>;

future<value_t> make_value() {
    return make_ready_future<value_t>(std::make_unique<int>(42));
}

} // anonymous namespace

SEASTAR_TEST_CASE(test_future_of_foreign_ptr_finally_compiles) {
    // The bug is at compile time; if this file compiled, the warning is
    // either present (regressed) or absent (still fixed). We also run
    // the chain so the destructor path is exercised at runtime.
    return make_value().finally([] {}).then([] (value_t v) {
        BOOST_REQUIRE(v);
    });
}
