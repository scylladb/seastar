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

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <vector>
#include <chrono>
#include <cstdlib>

#include <seastar/core/thread.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/testing/random.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/core/reactor.hh>
#include <seastar/util/later.hh>

using namespace seastar;

struct layout {
    std::vector<seastar::scheduling_supergroup> sgroups;
    std::vector<seastar::scheduling_group> groups;

    static future<layout> create();
    future<> destroy();
};

future<layout> layout::create() {
    layout ret;

    fmt::print("Creating supergroups\n");
    ret.sgroups.resize(2);
    ret.sgroups[0] = co_await seastar::create_scheduling_supergroup(100);
    ret.sgroups[1] = co_await seastar::create_scheduling_supergroup(100);

    fmt::print("Creating groups\n");
    ret.groups.resize(9);
    ret.groups[0] = co_await seastar::create_scheduling_group("g1", 100);
    ret.groups[1] = co_await seastar::create_scheduling_group("g2", 100);
    ret.groups[2] = co_await seastar::create_scheduling_group("g3", 100);
    ret.groups[3] = co_await seastar::create_scheduling_group("g11", "g11", 100, ret.sgroups[0]);
    ret.groups[4] = co_await seastar::create_scheduling_group("g21", "g21", 100, ret.sgroups[0]);
    ret.groups[5] = co_await seastar::create_scheduling_group("g31", "g31", 100, ret.sgroups[0]);
    ret.groups[6] = co_await seastar::create_scheduling_group("g12", "g12", 100, ret.sgroups[1]);
    ret.groups[7] = co_await seastar::create_scheduling_group("g22", "g22", 100, ret.sgroups[1]);
    ret.groups[8] = co_await seastar::create_scheduling_group("g32", "g32", 100, ret.sgroups[1]);

    co_return ret;
}

future<> layout::destroy() {
    for (auto sg : groups) {
        co_await seastar::destroy_scheduling_group(sg);
    }
    for (auto sg : sgroups) {
        co_await seastar::destroy_scheduling_supergroup(sg);
    }
}

SEASTAR_TEST_CASE(test_basic_ops) {
    auto l = co_await layout::create();
    BOOST_CHECK_EXCEPTION(co_await seastar::destroy_scheduling_supergroup(l.sgroups[0]), std::runtime_error, [] (const std::runtime_error& e) {
        return e.what() == sstring("Supergroup is still populated, destroy all subgroups first");
    });
    auto ssg = internal::scheduling_supergroup_for(l.groups[0]);
    BOOST_CHECK(ssg == scheduling_supergroup());
    ssg = internal::scheduling_supergroup_for(l.groups[6]);
    BOOST_CHECK(!ssg.is_root());
    BOOST_CHECK(ssg.index() == 1);
    BOOST_CHECK(ssg == l.sgroups[1]);
    co_await l.destroy();
}

#ifndef SEASTAR_SHUFFLE_TASK_QUEUE
// Default fairness deviation threshold
// Can be overridden at compile time with -DSEASTAR_SCHED_FAIRNESS_THRESHOLD=0.15
// or at runtime with SEASTAR_SCHED_FAIRNESS_THRESHOLD environment variable
#ifndef SEASTAR_SCHED_FAIRNESS_THRESHOLD
#define SEASTAR_SCHED_FAIRNESS_THRESHOLD 0.07
#endif

static float get_fairness_threshold() {
    static float threshold = [] {
        // Check environment variable for runtime override
        if (const char* env_threshold = std::getenv("SEASTAR_SCHED_FAIRNESS_THRESHOLD")) {
            try {
                float val = std::stof(env_threshold);
                if (val > 0.0f && val <= 1.0f) {
                    return val;
                }
                fmt::print("Warning: Invalid SEASTAR_SCHED_FAIRNESS_THRESHOLD value '{}', using default {:.2f}\n",
                           env_threshold, static_cast<float>(SEASTAR_SCHED_FAIRNESS_THRESHOLD));
            } catch (...) {
                fmt::print("Warning: Failed to parse SEASTAR_SCHED_FAIRNESS_THRESHOLD '{}', using default {:.2f}\n",
                           env_threshold, static_cast<float>(SEASTAR_SCHED_FAIRNESS_THRESHOLD));
            }
        }
        return static_cast<float>(SEASTAR_SCHED_FAIRNESS_THRESHOLD);
    }();
    return threshold;
}

static future<> run_busyloops(std::vector<seastar::scheduling_group> groups, std::vector<float> expected) {
    std::vector<uint64_t> counts;
    std::vector<future<>> f;
    auto run_and_count = [&groups] (uint64_t& c) -> future<> {
        unsigned total_run_ms = 100 * groups.size();
        auto now = std::chrono::steady_clock::now();
        auto start_count = now + std::chrono::milliseconds(20);
        auto stop_count = now + std::chrono::milliseconds(total_run_ms - 20);
        auto stop = now + std::chrono::milliseconds(total_run_ms);
        do {
            auto stop = std::chrono::steady_clock::now() + std::chrono::microseconds(10);
            while (std::chrono::steady_clock::now() < stop) ;

            auto now = std::chrono::steady_clock::now();
            if (now >= start_count && now < stop_count) {
                c++;
            }
            co_await yield();
        } while (std::chrono::steady_clock::now() < stop);
    };
    counts.reserve(groups.size());
    f.reserve(groups.size());
    for (auto& g : groups) {
        counts.push_back(0);
        f.push_back(with_scheduling_group(g, [&c = counts.back(), &run_and_count] { return run_and_count(c); }));
    }
    co_await when_all(f.begin(), f.end());

    uint64_t average_adjusted_count = 0;
    for (unsigned i = 0; i < groups.size(); i++) {
        average_adjusted_count += counts[i] / expected[i];
    }
    average_adjusted_count /= groups.size();

    fmt::print("--------8<--------\n");
    float threshold = get_fairness_threshold();
    for (unsigned i = 0; i < groups.size(); i++) {
        auto dev = float(std::abs(counts[i] / expected[i] - average_adjusted_count)) / average_adjusted_count;
        fmt::print("{}: count={} expected={:.2f} adjusted={} deviation={:.2f}\n", i, counts[i], expected[i], int(float(counts[i]) / expected[i]), dev);
        BOOST_CHECK(dev < threshold);
    }
}

static future<> do_test_fairness(layout& l) {
    std::default_random_engine random_engine(testing::local_random_engine());
    std::uniform_int_distribution<unsigned> ng_dist(2, 5);
    std::uniform_int_distribution<unsigned> shares_dist(1, 5);

    for (int test = 0; test < 8; test++) {
        fmt::print("=== TEST-{} ===\n", test);
        unsigned ngroups = ng_dist(random_engine);

        std::vector<unsigned> indices;
        indices.reserve(l.groups.size());
        for (unsigned i = 0; i < l.groups.size(); i++) {
            indices.push_back(i);
        }
        std::shuffle(indices.begin(), indices.end(), random_engine);
        indices.resize(ngroups);

        fmt::print("Running in {} groups: {}\n", ngroups, indices);

        std::vector<unsigned> run_shares;
        run_shares.reserve(ngroups);
        unsigned sg1_shares = 0, sg1_total_shares = 0;
        unsigned sg2_shares = 0, sg2_total_shares = 0;

        std::vector<seastar::scheduling_group> run_groups;
        run_groups.reserve(ngroups);

        unsigned root_total_shares = 0;
        for (unsigned i = 0; i < ngroups; i++) {
            unsigned idx = indices[i];
            auto sg = l.groups[idx];
            auto shares = shares_dist(random_engine);
            sg.set_shares(shares * 100);
            run_groups.push_back(sg);
            run_shares.push_back(shares);

            if (idx < 3) {
                root_total_shares += shares;
            } else if (idx < 6) {
                if (sg1_shares == 0) {
                    auto ssg = l.sgroups[0];
                    auto shares = shares_dist(random_engine);
                    ssg.set_shares(shares * 100);
                    sg1_shares = shares;
                    root_total_shares += shares;
                }
                sg1_total_shares += shares;
            } else {
                if (sg2_shares == 0) {
                    auto ssg = l.sgroups[1];
                    auto shares = shares_dist(random_engine);
                    ssg.set_shares(shares * 100);
                    sg2_shares = shares;
                    root_total_shares += shares;
                }
                sg2_total_shares += shares;
            }
        }

        fmt::print("Shares: {}\n", run_shares);
        if (sg1_shares != 0) {
            fmt::print("  supergroup 1 shares = {}\n", sg1_shares);
        }
        if (sg2_shares != 0) {
            fmt::print("  supergroup 2 shares = {}\n", sg2_shares);
        }

        std::vector<float> expectations;
        expectations.resize(ngroups);
        for (unsigned i = 0; i < ngroups; i++) {
            unsigned idx = indices[i];
            if (idx < 3) {
                expectations[i] = float(run_shares[i]) / root_total_shares;
            } else if (idx < 6) {
                float sg_expectation = float(sg1_shares) / root_total_shares;
                expectations[i] = float(run_shares[i]) / sg1_total_shares * sg_expectation;
            } else {
                float sg_expectation = float(sg2_shares) / root_total_shares;
                expectations[i] = float(run_shares[i]) / sg2_total_shares * sg_expectation;
            }
        }

        co_await run_busyloops(run_groups, expectations);
    }
}
#else
static future<> do_test_fairness(layout& l) {
    fmt::print("Skipping CPU fairness test in debug mode\n");
    return make_ready_future<>();
}
#endif

SEASTAR_TEST_CASE(test_fairness) {
    auto l = co_await layout::create();
    co_await do_test_fairness(l);
    co_await l.destroy();
}

SEASTAR_TEST_CASE(test_wakeups) {
    auto l = co_await layout::create();

    std::default_random_engine random_engine(testing::local_random_engine());
    std::uniform_int_distribution<unsigned> ng_dist(0, l.groups.size() - 1);
    std::uniform_int_distribution<unsigned> w_dist(2, 8);

    for (unsigned test = 0; test < 32; test++) {
        struct semaphore_and_number {
            semaphore sem;
            unsigned nr;
            semaphore_and_number(unsigned n) : sem(0), nr(n) {}
        };

        std::vector<semaphore_and_number> waiters;
        waiters.reserve(w_dist(random_engine));

        unsigned total_waiters = 0;
        semaphore ready(0);
        semaphore done(0);

        for (unsigned i = 0; i < waiters.capacity(); i++) {
            unsigned nw = w_dist(random_engine);
            fmt::print("Run with {} waiters\n", nw);
            total_waiters += nw;
            auto& ws = waiters.emplace_back(nw);

            for (unsigned w = 0; w < nw; w++) {
                auto g = ng_dist(random_engine);
                (void)with_scheduling_group(l.groups[g], [&s = ws.sem, &ready, &done, i, w, g] () mutable {
                    ready.signal();
                    return s.wait().then([&done, i, w, g] {
                        fmt::print("{}:{} wakeup in {}\n", i, w, g);
                        done.signal();
                    });
                });
            }
        }

        co_await ready.wait(total_waiters);

        for (unsigned i = 0; i < waiters.size(); i++) {
            auto g = ng_dist(random_engine);
            (void)with_scheduling_group(l.groups[g], [&s = waiters[i], &done] () mutable {
                s.sem.signal(s.nr);
                done.signal();
            });
        }

        co_await done.wait(total_waiters + waiters.size());
    }

    co_await l.destroy();
}
