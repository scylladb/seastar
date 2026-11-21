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
#include <fmt/chrono.h>
#include <vector>
#include <chrono>
#include <numeric>
#include <ranges>

#include <seastar/core/thread.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/testing/random.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/metrics_api.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/later.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/coroutine/parallel_for_each.hh>

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
    // A nested supergroup comes after its parent, so destroy from the back
    for (auto sg : sgroups | std::views::reverse) {
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

SEASTAR_TEST_CASE(test_nested_supergroup_ops) {
    auto top = co_await create_scheduling_supergroup(1000);
    auto nested = co_await create_scheduling_supergroup(1000, top);
    auto deeper = co_await create_scheduling_supergroup(1000, nested);
    auto sg = co_await create_scheduling_group("deeper", "dper", 100, deeper);

    // The IO scheduler uses the top-level supergroup, at any depth.
    BOOST_CHECK(internal::scheduling_supergroup_for(sg) == top);

    co_await destroy_scheduling_group(sg);
    BOOST_CHECK_EXCEPTION(co_await destroy_scheduling_supergroup(top), std::runtime_error, [] (const std::runtime_error& e) {
        return e.what() == sstring("Supergroup is still populated, destroy all subgroups first");
    });
    co_await destroy_scheduling_supergroup(deeper);
    co_await destroy_scheduling_supergroup(nested);
    co_await destroy_scheduling_supergroup(top);
    BOOST_CHECK_EXCEPTION(co_await create_scheduling_supergroup(100, top), std::runtime_error, [] (const std::runtime_error& e) {
        return e.what() == sstring("Requested supergroup doesn't exist");
    });
}

static metrics::histogram reactor_stalls() {
    const auto& values = metrics::impl::get_value_map();
    auto mf = values.find("reactor_stalls");
    if (mf == values.end()) {
        return {};
    }
    BOOST_REQUIRE_EQUAL(mf->second.size(), 1);
    return mf->second.begin()->second->get_function()().get_histogram();
}

static uint64_t stalls_longer_than(const metrics::histogram& h, std::chrono::microseconds duration) {
    uint64_t shorter = 0;
    for (const auto& b : h.buckets) {
        if (b.upper_bound <= duration.count()) {
            shorter = b.count;
        }
    }
    return h.sample_count - shorter;
}

static future<uint64_t> long_stalls_in(scheduling_group sg) {
    constexpr auto busy = std::chrono::milliseconds(50);
    auto before = stalls_longer_than(reactor_stalls(), busy / 2);
    co_await with_scheduling_group(sg, [busy] {
        auto stop = std::chrono::steady_clock::now() + busy;
        while (std::chrono::steady_clock::now() < stop) ;
    });
    co_return stalls_longer_than(reactor_stalls(), busy / 2) - before;
}

// Reproduces scylladb/seastar#3763
SEASTAR_TEST_CASE(test_stall_in_nested_group_is_counted_once) {
    auto ssg = co_await create_scheduling_supergroup(100);
    auto flat = co_await create_scheduling_group("flat", "flat", 100);
    auto nested = co_await create_scheduling_group("nested", "nstd", 100, ssg);

    auto flat_stalls = co_await long_stalls_in(flat);
    auto nested_stalls = co_await long_stalls_in(nested);

    co_await destroy_scheduling_group(nested);
    co_await destroy_scheduling_group(flat);
    co_await destroy_scheduling_supergroup(ssg);

    BOOST_REQUIRE_EQUAL(flat_stalls, 1);
    BOOST_CHECK_EQUAL(nested_stalls, 1);
}

#ifndef SEASTAR_SHUFFLE_TASK_QUEUE
static future<> run_busyloops(std::vector<seastar::scheduling_group> groups, std::vector<float> expected) {
    std::vector<uint64_t> counts;
    std::vector<future<>> f;
    bool keep_going = true;

    auto run_and_count = [&keep_going] (uint64_t& c) -> future<> {
        do {
            auto stop = std::chrono::steady_clock::now() + std::chrono::microseconds(10);
            while (std::chrono::steady_clock::now() < stop) ;
            c++;
            co_await yield();
        } while (keep_going);
    };
    counts.reserve(groups.size());
    f.reserve(groups.size());
    for (auto& g : groups) {
        counts.push_back(0);
        f.push_back(with_scheduling_group(g, [&c = counts.back(), &run_and_count] { return run_and_count(c); }));
    }

    auto warmup = std::chrono::milliseconds(20);
    fmt::print("Warmup for {}\n", warmup);
    co_await seastar::sleep(warmup);
    for (auto& c : counts) {
        c = 0;
    }

    auto sample_period = std::chrono::milliseconds(100);
    auto stop_at = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    bool ok = true;

  do {
    fmt::print("Do counting for {}\n", sample_period);
    co_await seastar::sleep(sample_period);

    uint64_t average_adjusted_count = 0;
    for (unsigned i = 0; i < groups.size(); i++) {
        average_adjusted_count += counts[i] / expected[i];
    }
    average_adjusted_count /= groups.size();

    ok = true;
    fmt::print("--------8<--------\n");
    for (unsigned i = 0; i < groups.size(); i++) {
        auto dev = float(std::abs(counts[i] / expected[i] - average_adjusted_count)) / average_adjusted_count;
        fmt::print("{}: count={} expected={:.2f} adjusted={} deviation={:.2f}\n", i, counts[i], expected[i], int(float(counts[i]) / expected[i]), dev);
        ok &= (dev < 0.07);
    }
    sample_period = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration_cast<std::chrono::duration<double>>(sample_period) * 1.618);
  } while (!ok && (std::chrono::steady_clock::now() <= stop_at));

    keep_going = false;
    co_await when_all(f.begin(), f.end());
    BOOST_REQUIRE(ok);
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

// Runs a busy loop until end and adds its wall time to busy. The scheduler charges
// wall time to a group, also when the OS gives the CPU to another process.
static future<> busy_loop(std::chrono::steady_clock::duration& busy, std::chrono::steady_clock::time_point end) {
    using clock = std::chrono::steady_clock;
    while (clock::now() < end) {
        auto start = clock::now();
        while (clock::now() - start < std::chrono::microseconds(50)) ;
        busy += clock::now() - start;
        co_await coroutine::maybe_yield();
    }
}

// Runs a busy loop in each group for 2 s and returns the fraction of the time that each group got.
static future<std::vector<double>> measure_fractions(std::vector<scheduling_group> groups) {
    using clock = std::chrono::steady_clock;
    std::vector<clock::duration> busy(groups.size());
    auto end = clock::now() + std::chrono::seconds(2);
    co_await coroutine::parallel_for_each(std::views::iota(size_t(0), groups.size()), [&] (size_t i) {
        return with_scheduling_group(groups[i], busy_loop, std::ref(busy[i]), end);
    });
    auto total = std::accumulate(busy.begin(), busy.end(), clock::duration{});
    std::vector<double> fractions;
    for (auto b : busy) {
        fractions.push_back(double(b.count()) / total.count());
    }
    co_return fractions;
}

static void check_fractions(const std::vector<double>& got, const std::vector<double>& expected, std::string_view what) {
    for (size_t i = 0; i < got.size(); i++) {
        fmt::print("{} {}: {:.3f} (expected {:.3f})\n", what, i, got[i], expected[i]);
        BOOST_CHECK_CLOSE(got[i], expected[i], 10.0);
    }
}

// root:   other 1000, top 1000
// top:    sibling 200, nested 1000
// nested: high 900, low 100
SEASTAR_TEST_CASE(test_nested_supergroup_fairness) {
    auto top = co_await create_scheduling_supergroup(1000);
    auto nested = co_await create_scheduling_supergroup(1000, top);
    auto other = co_await create_scheduling_group("other", "othr", 1000);
    auto sibling = co_await create_scheduling_group("sibling", "sibl", 200, top);
    auto high = co_await create_scheduling_group("high", "high", 900, nested);
    auto low = co_await create_scheduling_group("low", "low", 100, nested);

    // No work in high: low gets the whole part of nested.
    check_fractions(co_await measure_fractions({other, sibling, low}), {1. / 2, 1. / 12, 5. / 12}, "high idle");
    // All busy: nested keeps 1000/1200 of top, divided 900:100.
    check_fractions(co_await measure_fractions({other, sibling, high, low}), {1. / 2, 1. / 12, 3. / 8, 1. / 24}, "all busy");
    // set_shares() of nested changes its part of top, and not the division inside.
    nested.set_shares(500);
    check_fractions(co_await measure_fractions({other, sibling, high, low}), {1. / 2, 1. / 7, 9. / 28, 1. / 28}, "nested 500");

    for (auto sg : {other, sibling, high, low}) {
        co_await destroy_scheduling_group(sg);
    }
    co_await destroy_scheduling_supergroup(nested);
    co_await destroy_scheduling_supergroup(top);
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
    // Also wake up groups through two levels of supergroups
    l.sgroups.push_back(co_await seastar::create_scheduling_supergroup(100, l.sgroups[0]));
    l.groups.push_back(co_await seastar::create_scheduling_group("g41", "g41", 100, l.sgroups.back()));
    l.groups.push_back(co_await seastar::create_scheduling_group("g51", "g51", 100, l.sgroups.back()));

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

// Keep this test last: it leaves a nested supergroup alive at exit, so the reactor
// destroys it after its parent, which has the lower index in the supergroup list.
SEASTAR_TEST_CASE(test_nested_supergroup_alive_at_exit) {
    auto top = co_await create_scheduling_supergroup(1000);
    co_await create_scheduling_supergroup(1000, top);
}
