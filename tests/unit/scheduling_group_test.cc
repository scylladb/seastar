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
 * Copyright (C) 2017 ScyllaDB Ltd.
 */

#include <algorithm>
#include <vector>
#include <chrono>

#include <seastar/core/thread.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/core/execution_stage.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/print.hh>
#include <seastar/core/scheduling_specific.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/core/reactor.hh>
#include <seastar/util/later.hh>
#include <seastar/util/defer.hh>

using namespace std::chrono_literals;

using namespace seastar;

/**
 *  Test setting primitive and object as a value after all groups are created
 */
SEASTAR_THREAD_TEST_CASE(sg_specific_values_define_after_sg_create) {
    using ivec  = std::vector<int>;
    const int num_scheduling_groups = 4;
    std::vector<scheduling_group> sgs;
    for (int i = 0; i < num_scheduling_groups; i++) {
        sgs.push_back(create_scheduling_group(format("sg{}", i).c_str(), 100).get0());
    }

    const auto destroy_scheduling_groups = defer([&sgs] () noexcept {
       for (scheduling_group sg : sgs) {
           destroy_scheduling_group(sg).get();
       }
    });
    scheduling_group_key_config key1_conf = make_scheduling_group_key_config<int>();
    scheduling_group_key key1 = scheduling_group_key_create(key1_conf).get0();

    scheduling_group_key_config key2_conf = make_scheduling_group_key_config<ivec>();
    scheduling_group_key key2 = scheduling_group_key_create(key2_conf).get0();

    smp::invoke_on_all([key1, key2, &sgs] () {
        int factor = this_shard_id() + 1;
        for (int i=0; i < num_scheduling_groups; i++) {
            sgs[i].get_specific<int>(key1) = (i + 1) * factor;
            sgs[i].get_specific<ivec>(key2).push_back((i + 1) * factor);
        }

        for (int i=0; i < num_scheduling_groups; i++) {
            BOOST_REQUIRE_EQUAL(sgs[i].get_specific<int>(key1) = (i + 1) * factor, (i + 1) * factor);
            BOOST_REQUIRE_EQUAL(sgs[i].get_specific<ivec>(key2)[0], (i + 1) * factor);
        }

    }).get();

    smp::invoke_on_all([key1, key2] () {
        return reduce_scheduling_group_specific<int>(std::plus<int>(), int(0), key1).then([] (int sum) {
            int factor = this_shard_id() + 1;
            int expected_sum = ((1 + num_scheduling_groups)*num_scheduling_groups) * factor /2;
            BOOST_REQUIRE_EQUAL(expected_sum, sum);
        }). then([key2] {
            auto ivec_to_int = [] (ivec& v) {
                return v.size() ? v[0] : 0;
            };

            return map_reduce_scheduling_group_specific<ivec>(ivec_to_int, std::plus<int>(), int(0), key2).then([] (int sum) {
                int factor = this_shard_id() + 1;
                int expected_sum = ((1 + num_scheduling_groups)*num_scheduling_groups) * factor /2;
                BOOST_REQUIRE_EQUAL(expected_sum, sum);
            });

        });
    }).get();


}

/**
 *  Test setting primitive and object as a value before all groups are created
 */
SEASTAR_THREAD_TEST_CASE(sg_specific_values_define_before_sg_create) {
    using ivec  = std::vector<int>;
    const int num_scheduling_groups = 4;
    std::vector<scheduling_group> sgs;
    const auto destroy_scheduling_groups = defer([&sgs] () noexcept {
       for (scheduling_group sg : sgs) {
           destroy_scheduling_group(sg).get();
       }
    });
    scheduling_group_key_config key1_conf = make_scheduling_group_key_config<int>();
    scheduling_group_key key1 = scheduling_group_key_create(key1_conf).get0();

    scheduling_group_key_config key2_conf = make_scheduling_group_key_config<ivec>();
    scheduling_group_key key2 = scheduling_group_key_create(key2_conf).get0();

    for (int i = 0; i < num_scheduling_groups; i++) {
        sgs.push_back(create_scheduling_group(format("sg{}", i).c_str(), 100).get0());
    }

    smp::invoke_on_all([key1, key2, &sgs] () {
        int factor = this_shard_id() + 1;
        for (int i=0; i < num_scheduling_groups; i++) {
            sgs[i].get_specific<int>(key1) = (i + 1) * factor;
            sgs[i].get_specific<ivec>(key2).push_back((i + 1) * factor);
        }

        for (int i=0; i < num_scheduling_groups; i++) {
            BOOST_REQUIRE_EQUAL(sgs[i].get_specific<int>(key1) = (i + 1) * factor, (i + 1) * factor);
            BOOST_REQUIRE_EQUAL(sgs[i].get_specific<ivec>(key2)[0], (i + 1) * factor);
        }

    }).get();

    smp::invoke_on_all([key1, key2] () {
        return reduce_scheduling_group_specific<int>(std::plus<int>(), int(0), key1).then([] (int sum) {
            int factor = this_shard_id() + 1;
            int expected_sum = ((1 + num_scheduling_groups)*num_scheduling_groups) * factor /2;
            BOOST_REQUIRE_EQUAL(expected_sum, sum);
        }). then([key2] {
            auto ivec_to_int = [] (ivec& v) {
                return v.size() ? v[0] : 0;
            };

            return map_reduce_scheduling_group_specific<ivec>(ivec_to_int, std::plus<int>(), int(0), key2).then([] (int sum) {
                int factor = this_shard_id() + 1;
                int expected_sum = ((1 + num_scheduling_groups)*num_scheduling_groups) * factor /2;
                BOOST_REQUIRE_EQUAL(expected_sum, sum);
            });

        });
    }).get();

}

/**
 *  Test setting primitive and an object as a value before some groups are created
 *  and after some of the groups are created.
 */
SEASTAR_THREAD_TEST_CASE(sg_specific_values_define_before_and_after_sg_create) {
    using ivec  = std::vector<int>;
    const int num_scheduling_groups = 4;
    std::vector<scheduling_group> sgs;
    const auto destroy_scheduling_groups = defer([&sgs] () noexcept {
       for (scheduling_group sg : sgs) {
           destroy_scheduling_group(sg).get();
       }
    });

    for (int i = 0; i < num_scheduling_groups/2; i++) {
        sgs.push_back(create_scheduling_group(format("sg{}", i).c_str(), 100).get0());
    }
    scheduling_group_key_config key1_conf = make_scheduling_group_key_config<int>();
    scheduling_group_key key1 = scheduling_group_key_create(key1_conf).get0();

    scheduling_group_key_config key2_conf = make_scheduling_group_key_config<ivec>();
    scheduling_group_key key2 = scheduling_group_key_create(key2_conf).get0();

    for (int i = num_scheduling_groups/2; i < num_scheduling_groups; i++) {
        sgs.push_back(create_scheduling_group(format("sg{}", i).c_str(), 100).get0());
    }

    smp::invoke_on_all([key1, key2, &sgs] () {
        int factor = this_shard_id() + 1;
        for (int i=0; i < num_scheduling_groups; i++) {
            sgs[i].get_specific<int>(key1) = (i + 1) * factor;
            sgs[i].get_specific<ivec>(key2).push_back((i + 1) * factor);
        }

        for (int i=0; i < num_scheduling_groups; i++) {
            BOOST_REQUIRE_EQUAL(sgs[i].get_specific<int>(key1) = (i + 1) * factor, (i + 1) * factor);
            BOOST_REQUIRE_EQUAL(sgs[i].get_specific<ivec>(key2)[0], (i + 1) * factor);
        }

    }).get();

    smp::invoke_on_all([key1, key2] () {
        return reduce_scheduling_group_specific<int>(std::plus<int>(), int(0), key1).then([] (int sum) {
            int factor = this_shard_id() + 1;
            int expected_sum = ((1 + num_scheduling_groups)*num_scheduling_groups) * factor /2;
            BOOST_REQUIRE_EQUAL(expected_sum, sum);
        }). then([key2] {
            auto ivec_to_int = [] (ivec& v) {
                return v.size() ? v[0] : 0;
            };

            return map_reduce_scheduling_group_specific<ivec>(ivec_to_int, std::plus<int>(), int(0), key2).then([] (int sum) {
                int factor = this_shard_id() + 1;
                int expected_sum = ((1 + num_scheduling_groups)*num_scheduling_groups) * factor /2;
                BOOST_REQUIRE_EQUAL(expected_sum, sum);
            });

        });
    }).get();
}

/*
 * Test that current scheduling group is inherited by seastar::async()
 */
SEASTAR_THREAD_TEST_CASE(sg_scheduling_group_inheritance_in_seastar_async_test) {
    scheduling_group sg = create_scheduling_group("sg0", 100).get0();
    auto cleanup = defer([&] () noexcept { destroy_scheduling_group(sg).get(); });
    thread_attributes attr = {};
    attr.sched_group = sg;
    seastar::async(attr, [attr] {
        BOOST_REQUIRE_EQUAL(internal::scheduling_group_index(current_scheduling_group()),
                                internal::scheduling_group_index(*(attr.sched_group)));

        seastar::async([attr] {
            BOOST_REQUIRE_EQUAL(internal::scheduling_group_index(current_scheduling_group()),
                                internal::scheduling_group_index(*(attr.sched_group)));

            smp::invoke_on_all([sched_group_idx = internal::scheduling_group_index(*(attr.sched_group))] () {
                BOOST_REQUIRE_EQUAL(internal::scheduling_group_index(current_scheduling_group()), sched_group_idx);
            }).get();
        }).get();
    }).get();
}


SEASTAR_THREAD_TEST_CASE(yield_preserves_sg) {
    scheduling_group sg = create_scheduling_group("sg", 100).get0();
    auto cleanup = defer([&] () noexcept { destroy_scheduling_group(sg).get(); });
    with_scheduling_group(sg, [&] {
        return yield().then([&] {
            BOOST_REQUIRE_EQUAL(
                    internal::scheduling_group_index(current_scheduling_group()),
                    internal::scheduling_group_index(sg));
        });
    }).get();
}

SEASTAR_THREAD_TEST_CASE(sg_count) {
    class scheduling_group_destroyer {
        scheduling_group _sg;
    public:
        scheduling_group_destroyer(scheduling_group sg) : _sg(sg) {}
        ~scheduling_group_destroyer() {
            destroy_scheduling_group(_sg).get();
        }
    };

    std::vector<scheduling_group_destroyer> scheduling_groups_deferred_cleanup;
    // The line below is necessary in order to skip support of copy and move construction of scheduling_group_destroyer.
    scheduling_groups_deferred_cleanup.reserve(max_scheduling_groups());
    // try to create 3 groups too many.
    for (auto i = internal::scheduling_group_count(); i < max_scheduling_groups() + 3; i++) {
        try {
            BOOST_REQUIRE_LE(internal::scheduling_group_count(), max_scheduling_groups());
            scheduling_groups_deferred_cleanup.emplace_back(create_scheduling_group(format("sg_{}", i), 10).get());
        } catch (std::runtime_error& e) {
            // make sure it is the right exception.
            BOOST_REQUIRE_EQUAL(e.what(), fmt::format("Scheduling group limit exceeded while creating sg_{}", i));
            // make sure that the scheduling group count makes sense
            BOOST_REQUIRE_EQUAL(internal::scheduling_group_count(), max_scheduling_groups());
            // make sure that we expect this exception at this point
            BOOST_REQUIRE_GE(i, max_scheduling_groups());
        }
    }
    BOOST_REQUIRE_EQUAL(internal::scheduling_group_count(), max_scheduling_groups());
}
