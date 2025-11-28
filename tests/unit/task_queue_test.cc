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
 * Copyright (C) 2025 ScyllaDB Ltd.
 */


#define BOOST_TEST_MODULE core

#include <boost/test/unit_test.hpp>
#include <stdlib.h>
#include <random>
#include <deque>

#include <seastar/core/task.hh>
#include <seastar/util/assert.hh>

using namespace seastar;

class dummy_task final : public task {
    const unsigned _id;
public:
    void run_and_dispose() noexcept override { delete this; }
    task* waiting_task() noexcept override { return nullptr; }
    dummy_task(unsigned id) noexcept : task(scheduling_group()), _id(id) {}
    unsigned id() const noexcept { return _id; }
};

// A fuzzy test that validates task_slist modifications
// Takes a std::deque as a reference container and randomly pushes/pops/checks
// both, deque and slist, verifying that their contents remains equal
BOOST_AUTO_TEST_CASE(test_task_slist_grow_and_shrink) {
    task_slist list;
    std::deque<unsigned> ids;

    const auto seed = std::random_device()();
    auto rnd_engine = std::mt19937(seed);
    std::uniform_int_distribution<unsigned> op(0, 3);

    fmt::print("start\n");
    BOOST_REQUIRE(list.empty());

    unsigned tid = 0;
    while (tid < 1573) {
        auto o = op(rnd_engine);
        if (o == 0) {
            fmt::print("+>{}\n", tid);
            ids.push_back(tid);
            list.push_back(new dummy_task(tid));
            tid++;
        } else if (o == 1) {
            fmt::print("+<{}\n", tid);
            ids.push_front(tid);
            list.push_front(new dummy_task(tid));
            tid++;
        } else if (o == 2) {
            fmt::print("--\n");
            if (list.empty()) {
                BOOST_REQUIRE(ids.empty());
            } else {
                task* t = list.pop_front(current_scheduling_group());
                BOOST_REQUIRE_EQUAL(reinterpret_cast<dummy_task*>(t)->id(), ids.front());
                t->run_and_dispose();
                ids.pop_front();
            }
        } else {
            fmt::print("??\n");
            auto idp = ids.begin();
            list.do_for_each([&] (const task* t) {
                BOOST_REQUIRE_EQUAL(reinterpret_cast<const dummy_task*>(t)->id(), *idp);
                idp++;
            });
        }
        BOOST_REQUIRE_EQUAL(list.size(), ids.size());
        BOOST_REQUIRE_EQUAL(list.empty(), ids.empty());
    }

    while (!list.empty()) {
        task* t = list.pop_front(current_scheduling_group());
        t->run_and_dispose();
    }
}
