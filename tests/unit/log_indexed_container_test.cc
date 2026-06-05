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
 * Copyright (C) 2026-present ScyllaDB
 */

#define BOOST_TEST_MODULE log_indexed_container
#include <boost/test/unit_test.hpp>

#include <memory>
#include <vector>

#include <seastar/util/log_indexed_container.hh>

using seastar::log_indexed_container;

// ── Tests using int (optional<int> slots) ────────────────────────────────────

BOOST_AUTO_TEST_CASE(test_empty_container) {
    log_indexed_container<int> c;
    BOOST_CHECK(c.empty());
    BOOST_CHECK(c.find(0) == nullptr);
    BOOST_CHECK(c.find(100) == nullptr);
}

BOOST_AUTO_TEST_CASE(test_emplace_and_find) {
    log_indexed_container<int> c;
    c.emplace(5, 42);
    BOOST_CHECK(!c.empty());
    BOOST_CHECK_EQUAL(c.base_index(), 5u);

    auto* p = c.find(5);
    BOOST_REQUIRE(p != nullptr);
    BOOST_CHECK_EQUAL(*p, 42);

    BOOST_CHECK(c.find(4) == nullptr);
    BOOST_CHECK(c.find(6) == nullptr);
}

BOOST_AUTO_TEST_CASE(test_emplace_with_gap) {
    log_indexed_container<int> c;
    c.emplace(10, 1);
    c.emplace(13, 2);

    BOOST_CHECK_EQUAL(*c.find(10), 1);
    BOOST_CHECK(c.find(11) == nullptr);
    BOOST_CHECK(c.find(12) == nullptr);
    BOOST_CHECK_EQUAL(*c.find(13), 2);
}

BOOST_AUTO_TEST_CASE(test_emplace_before_base) {
    log_indexed_container<int> c;
    c.emplace(5, 50);
    c.emplace(7, 70);

    // Emplace before the current base index — empty slots prepended.
    c.emplace(3, 30);
    BOOST_CHECK_EQUAL(c.base_index(), 3u);
    BOOST_CHECK_EQUAL(*c.find(3), 30);
    BOOST_CHECK(c.find(4) == nullptr);
    BOOST_CHECK_EQUAL(*c.find(5), 50);
    BOOST_CHECK(c.find(6) == nullptr);
    BOOST_CHECK_EQUAL(*c.find(7), 70);
}

BOOST_AUTO_TEST_CASE(test_clear_at_and_trim) {
    log_indexed_container<int> c;
    c.emplace(5, 1);
    c.emplace(6, 2);
    c.emplace(7, 3);

    c.clear_at(5);
    c.trim_front();
    // Front cleared: base_index advances to 6.
    BOOST_CHECK_EQUAL(c.base_index(), 6u);
    BOOST_CHECK(c.find(5) == nullptr);
    BOOST_CHECK_EQUAL(*c.find(6), 2);

    c.clear_at(7);
    c.trim_front();
    // Index 7 cleared but 6 still at front — no advancement.
    BOOST_CHECK_EQUAL(c.base_index(), 6u);
    BOOST_CHECK(c.find(7) == nullptr);

    c.clear_at(6);
    c.trim_front();
    // All cleared: container empty.
    BOOST_CHECK(c.empty());
}

BOOST_AUTO_TEST_CASE(test_clear_at_middle_then_front) {
    log_indexed_container<int> c;
    c.emplace(1, 10);
    c.emplace(2, 20);
    c.emplace(3, 30);

    // Clear middle — trim stops at front.
    c.clear_at(2);
    c.trim_front();
    BOOST_CHECK_EQUAL(c.base_index(), 1u);
    BOOST_CHECK(c.find(2) == nullptr);

    // Clear front — trims past the gap to 3.
    c.clear_at(1);
    c.trim_front();
    BOOST_CHECK_EQUAL(c.base_index(), 3u);
    BOOST_CHECK_EQUAL(*c.find(3), 30);
}

BOOST_AUTO_TEST_CASE(test_for_each) {
    log_indexed_container<int> c;
    c.emplace(2, 20);
    c.emplace(4, 40);
    c.emplace(5, 50);

    std::vector<std::pair<size_t, int>> visited;
    c.for_each([&](size_t idx, int& val) {
        visited.emplace_back(idx, val);
    });

    BOOST_REQUIRE_EQUAL(visited.size(), 3u);
    BOOST_CHECK_EQUAL(visited[0].first, 2u);
    BOOST_CHECK_EQUAL(visited[0].second, 20);
    BOOST_CHECK_EQUAL(visited[1].first, 4u);
    BOOST_CHECK_EQUAL(visited[1].second, 40);
    BOOST_CHECK_EQUAL(visited[2].first, 5u);
    BOOST_CHECK_EQUAL(visited[2].second, 50);
}

BOOST_AUTO_TEST_CASE(test_for_each_with_clear_at) {
    log_indexed_container<int> c;
    c.emplace(1, 10);
    c.emplace(2, 20);
    c.emplace(3, 30);

    // Clear some entries during iteration.
    c.for_each([&](size_t idx, int& /*val*/) {
        if (idx == 1 || idx == 3) {
            c.clear_at(idx);
        }
    });

    // for_each calls trim_front: 1 cleared at front, 2 retained, 3 cleared.
    BOOST_CHECK(c.find(1) == nullptr);
    BOOST_CHECK_EQUAL(*c.find(2), 20);
    BOOST_CHECK(c.find(3) == nullptr);
}

BOOST_AUTO_TEST_CASE(test_clear_and_reuse) {
    log_indexed_container<int> c;
    c.emplace(10, 1);
    c.emplace(20, 2);

    c.clear();
    BOOST_CHECK(c.empty());
    BOOST_CHECK(c.find(10) == nullptr);

    // Can reuse after clear.
    c.emplace(5, 99);
    BOOST_CHECK_EQUAL(*c.find(5), 99);
}

// ── Tests using unique_ptr<int> (nullable — no optional wrapper) ──────────────

BOOST_AUTO_TEST_CASE(test_move_only_type) {
    log_indexed_container<std::unique_ptr<int>> c;
    c.emplace(1, std::make_unique<int>(42));

    auto* p = c.find(1);
    BOOST_REQUIRE(p != nullptr);
    BOOST_CHECK_EQUAL(**p, 42);

    c.clear_at(1);
    c.trim_front();
    BOOST_CHECK(c.empty());
}

BOOST_AUTO_TEST_CASE(test_unique_ptr_emplace_and_find) {
    log_indexed_container<std::unique_ptr<int>> c;
    c.emplace(3, std::make_unique<int>(10));
    c.emplace(5, std::make_unique<int>(20));

    BOOST_REQUIRE(c.find(3) != nullptr);
    BOOST_CHECK_EQUAL(*(*c.find(3)), 10);
    BOOST_CHECK(c.find(4) == nullptr);
    BOOST_REQUIRE(c.find(5) != nullptr);
    BOOST_CHECK_EQUAL(*(*c.find(5)), 20);
}

BOOST_AUTO_TEST_CASE(test_unique_ptr_for_each_with_clear) {
    log_indexed_container<std::unique_ptr<int>> c;
    c.emplace(10, std::make_unique<int>(1));
    c.emplace(11, std::make_unique<int>(2));
    c.emplace(12, std::make_unique<int>(3));

    std::vector<int> seen;
    c.for_each([&](size_t idx, std::unique_ptr<int>& val) {
        seen.push_back(*val);
        c.clear_at(idx);
    });

    BOOST_REQUIRE_EQUAL(seen.size(), 3u);
    BOOST_CHECK_EQUAL(seen[0], 1);
    BOOST_CHECK_EQUAL(seen[1], 2);
    BOOST_CHECK_EQUAL(seen[2], 3);
    BOOST_CHECK(c.empty());
}

// ── Tests using a custom (non-size_t) IndexType ───────────────────────────────

BOOST_AUTO_TEST_CASE(test_custom_index_type) {
    // Use int64_t as IndexType — matches the rpc::client id_type use case.
    log_indexed_container<int, int64_t> c;

    c.emplace(int64_t{1}, 100);
    c.emplace(int64_t{3}, 300);

    BOOST_CHECK_EQUAL(c.base_index(), int64_t{1});
    BOOST_REQUIRE(c.find(int64_t{1}) != nullptr);
    BOOST_CHECK_EQUAL(*c.find(int64_t{1}), 100);
    BOOST_CHECK(c.find(int64_t{2}) == nullptr);
    BOOST_REQUIRE(c.find(int64_t{3}) != nullptr);
    BOOST_CHECK_EQUAL(*c.find(int64_t{3}), 300);

    c.clear_at(int64_t{1});
    c.trim_front();
    // Slot for id=1 cleared; trim also skips the null gap at id=2, so base advances to 3.
    BOOST_CHECK_EQUAL(c.base_index(), int64_t{3});
    BOOST_REQUIRE(c.find(int64_t{3}) != nullptr);
    BOOST_CHECK_EQUAL(*c.find(int64_t{3}), 300);
}
