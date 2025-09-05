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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sleep.hh>
#include <iostream>

using namespace seastar;

namespace seastar {

extern logger seastar_logger;

}

SEASTAR_TEST_CASE(make_foreign_ptr_from_lw_shared_ptr) {
    auto p = make_foreign(make_lw_shared<sstring>("foo"));
    BOOST_REQUIRE(p->size() == 3);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(make_foreign_ptr_from_shared_ptr) {
    auto p = make_foreign(make_shared<sstring>("foo"));
    BOOST_REQUIRE(p->size() == 3);
    return make_ready_future<>();
}


SEASTAR_TEST_CASE(foreign_ptr_copy_test) {
    return seastar::async([] {
        auto ptr = make_foreign(make_shared<sstring>("foo"));
        BOOST_REQUIRE(ptr->size() == 3);
        auto ptr2 = ptr.copy().get();
        BOOST_REQUIRE(ptr2->size() == 3);
    });
}

SEASTAR_TEST_CASE(foreign_ptr_get_test) {
    auto p = make_foreign(std::make_unique<sstring>("foo"));
    BOOST_REQUIRE_EQUAL(p.get(), &*p);
    return make_ready_future<>();
};

SEASTAR_TEST_CASE(foreign_ptr_release_test) {
    auto p = make_foreign(std::make_unique<sstring>("foo"));
    auto raw_ptr = p.get();
    BOOST_REQUIRE(bool(p));
    BOOST_REQUIRE(p->size() == 3);
    auto released_p = p.release();
    BOOST_REQUIRE(!bool(p));
    BOOST_REQUIRE(released_p->size() == 3);
    BOOST_REQUIRE_EQUAL(raw_ptr, released_p.get());
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(foreign_ptr_reset_test) {
    auto fp = make_foreign(std::make_unique<sstring>("foo"));
    BOOST_REQUIRE(bool(fp));
    BOOST_REQUIRE(fp->size() == 3);

    fp.reset(std::make_unique<sstring>("foobar"));
    BOOST_REQUIRE(bool(fp));
    BOOST_REQUIRE(fp->size() == 6);

    fp.reset();
    BOOST_REQUIRE(!bool(fp));
    return make_ready_future<>();
}

class dummy {
    unsigned _cpu;
public:
    dummy() : _cpu(this_shard_id()) { }
    ~dummy() { BOOST_REQUIRE_EQUAL(_cpu, this_shard_id()); }
};

SEASTAR_TEST_CASE(foreign_ptr_cpu_test) {
    if (smp::count == 1) {
        std::cerr << "Skipping multi-cpu foreign_ptr tests. Run with --smp=2 to test multi-cpu delete and reset.";
        return make_ready_future<>();
    }

    using namespace std::chrono_literals;

    return seastar::async([] {
        auto p = smp::submit_to(1, [] {
            return make_foreign(std::make_unique<dummy>());
        }).get();

        p.reset(std::make_unique<dummy>());
    }).then([] {
        // Let ~foreign_ptr() take its course. RIP dummy.
        return seastar::sleep(100ms);
    });
}

SEASTAR_TEST_CASE(foreign_ptr_move_assignment_test) {
    if (smp::count == 1) {
        std::cerr << "Skipping multi-cpu foreign_ptr tests. Run with --smp=2 to test multi-cpu delete and reset.";
        return make_ready_future<>();
    }

    using namespace std::chrono_literals;

    return seastar::async([] {
        auto p = smp::submit_to(1, [] {
            return make_foreign(std::make_unique<dummy>());
        }).get();

        p = foreign_ptr<std::unique_ptr<dummy>>();
    }).then([] {
        // Let ~foreign_ptr() take its course. RIP dummy.
        return seastar::sleep(100ms);
    });
}

SEASTAR_THREAD_TEST_CASE(foreign_ptr_destroy_test) {
    if (smp::count == 1) {
        std::cerr << "Skipping multi-cpu foreign_ptr tests. Run with --smp=2 to test multi-cpu delete and reset.";
        return;
    }

    using namespace std::chrono_literals;

    std::vector<promise<bool>> done;
    done.resize(smp::count);

    struct deferred {
        std::vector<promise<bool>>& done;
        deferred(std::vector<promise<bool>>& done_)
            : done(done_)
        {}
        ~deferred() {
            seastar_logger.info("~deferred");
            internal::run_in_background([&done = done, shard = this_shard_id()] {
                return smp::submit_to(0, [&done, shard] {
                    done[shard].set_value(true);
                    done[shard ^ 1].set_value(false);
                });
            });
        }
    };

    auto val = smp::submit_to(1, [&] () mutable {
        return make_foreign(std::make_unique<deferred>(done));
    }).get();

    val.destroy().get();

    BOOST_REQUIRE_EQUAL(done[1].get_future().get(), true);
    BOOST_REQUIRE_EQUAL(done[0].get_future().get(), false);
}

SEASTAR_THREAD_TEST_CASE(test_foreign_ptr_use_count) {
    shard_id shard = (this_shard_id() + 1) % smp::count;
    auto p0 = smp::submit_to(shard, [] {
        return make_foreign(make_lw_shared<sstring>("foo"));
    }).get();
    smp::submit_to(shard, [&] {
        BOOST_REQUIRE_EQUAL(p0.unwrap_on_owner_shard().use_count(), 1);
    }).get();
    auto p1 = p0.copy().get();
    smp::submit_to(shard, [&] {
        BOOST_REQUIRE_EQUAL(p0.unwrap_on_owner_shard().use_count(), 2);
    }).get();
    smp::submit_to(shard, [&] {
        auto ptr = p0.release();
        BOOST_REQUIRE_EQUAL(p0.unwrap_on_owner_shard().use_count(), 0);
        BOOST_REQUIRE_EQUAL(p1.unwrap_on_owner_shard().use_count(), 2);
        ptr = {};
        BOOST_REQUIRE_EQUAL(p1.unwrap_on_owner_shard().use_count(), 1);
    }).get();
    p1.reset();
    smp::submit_to(shard, [&] {
        BOOST_REQUIRE_EQUAL(p0.unwrap_on_owner_shard().use_count(), 0);
    }).get();
}
