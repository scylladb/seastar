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

#include <seastar/core/distributed.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sleep.hh>
#include <iostream>

using namespace seastar;

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
        auto ptr2 = ptr.copy().get0();
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
        }).get0();

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
        }).get0();

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

    std::vector<bool> destroyed_on;
    destroyed_on.resize(smp::count);

    struct deferred {
        std::function<void()> on_destroy;
        deferred(std::function<void()> on_destroy_func)
            : on_destroy(std::move(on_destroy_func))
        {}
        ~deferred() {
            on_destroy();
        }
    };

    auto val = smp::submit_to(1, [&] () mutable {
        return make_foreign(std::make_unique<deferred>([&] {
            destroyed_on[this_shard_id()] = true;
        }));
    }).get0();

    val.destroy().get();

    BOOST_REQUIRE(destroyed_on[1]);
    BOOST_REQUIRE(!destroyed_on[0]);
}
