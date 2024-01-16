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
#define BOOST_TEST_MODULE core

#include <boost/test/unit_test.hpp>
#include <seastar/util/trie.hh>

using namespace seastar;

BOOST_AUTO_TEST_CASE(test_single_equality) {
    trie<int> trie;
    trie.put("114514", 1);
    BOOST_REQUIRE_EQUAL(trie.get("114514")->getValue(), 1);
}

BOOST_AUTO_TEST_CASE(test_multi_equality) {
    trie<int> trie;
    trie.put("114514", 1);
    trie.put("1919810", 2);
    trie.put("11451", 3);
    BOOST_REQUIRE_EQUAL(trie.get("114514")->getValue(), 1);
    BOOST_REQUIRE_EQUAL(trie.get("1919810")->getValue(), 2);
    BOOST_REQUIRE_EQUAL(trie.get("11451")->getValue(), 3);
}

BOOST_AUTO_TEST_CASE(test_nullptr) {
    trie<int> trie;
    BOOST_REQUIRE_EQUAL(trie.get("114514"), nullptr);
}