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
 * Copyright (C) 2016 ScyllaDB
 */

#include <seastar/core/thread.hh>
#include <seastar/testing/random.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/fair_queue.hh>
#include <seastar/core/do_with.hh>
#include <seastar/util/later.hh>
#include <seastar/util/assert.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/print.hh>
#include <boost/range/irange.hpp>
#include <chrono>

using namespace seastar;
using namespace std::chrono_literals;

struct request {
    fair_queue_entry fqent;
    std::function<void(request& req)> handle;
    unsigned index;

    template <typename Func>
    request(fair_queue_entry::capacity_t cap, unsigned index, Func&& h)
        : fqent(cap)
        , handle(std::move(h))
        , index(index)
    {}

    void submit() {
        handle(*this);
        delete this;
    }
};

constexpr unsigned test_weight_scale = 1000;

class test_env {
    fair_queue _fq;
    std::vector<int> _results;
    std::vector<std::vector<std::exception_ptr>> _exceptions;
    fair_queue::class_id _nr_classes = 0;
    unsigned _nr_groups = 0;
    std::vector<request> _inflight;

    static fair_queue::config fq_config() {
        fair_queue::config cfg;
        cfg.forgiving_factor = 50 * test_weight_scale;
        return cfg;
    }

    void drain() {
        do {} while (tick() != 0);
    }
public:
    test_env()
        : _fq(fq_config())
    {
    }

    // As long as there is a request sitting in the queue, tick() will process
    // at least one request. The only situation in which tick() will return nothing
    // is if no requests were sent to the fair_queue (obviously).
    //
    // Because of this property, one useful use of tick() is to implement a drain()
    // method (see above) in which all requests currently sent to the queue are drained
    // before the queue is destroyed.
    unsigned tick(unsigned n = 1) {
        unsigned dispatched = 0;
        unsigned processed = 0;
        while (dispatched < n) {
            auto* req = _fq.top();
            if (req == nullptr) {
                break;
            }

            dispatched++;
            _fq.pop_front();
            boost::intrusive::get_parent_from_member(req, &request::fqent)->submit();
        }

        for (unsigned i = 0; i < n; ++i) {
            std::vector<request> curr;
            curr.swap(_inflight);

            for (auto& req : curr) {
                processed++;
                _results[req.index]++;
                _fq.notify_request_finished(req.fqent.capacity());
            }
        }
        return processed;
    }

    ~test_env() {
        drain();
        for (fair_queue::class_id id = 0; id < _nr_classes; id++) {
            _fq.unregister_priority_class(id);
        }
    }

    unsigned register_priority_group(uint32_t shares) {
        _fq.ensure_priority_group(_nr_groups, shares);
        return _nr_groups++;
    }

    size_t register_priority_class(uint32_t shares, std::optional<unsigned> group = {}) {
        _results.push_back(0);
        _exceptions.push_back(std::vector<std::exception_ptr>());
        _fq.register_priority_class(_nr_classes, shares, group);
        return _nr_classes++;
    }

    void do_op(fair_queue::class_id id, unsigned weight) {
        unsigned index = id;
        auto cap = fair_queue_entry::capacity_t(test_weight_scale * weight);
        auto req = std::make_unique<request>(cap, index, [this, index] (request& req) mutable noexcept {
            try {
                _inflight.push_back(std::move(req));
            } catch (...) {
                auto eptr = std::current_exception();
                _exceptions[index].push_back(eptr);
                _fq.notify_request_finished(req.fqent.capacity());
            }
        });

        _fq.queue(id, req->fqent);
        req.release();
    }

    void update_shares(fair_queue::class_id id, uint32_t shares) {
        _fq.update_shares_for_class(id, shares);
    }

    void reset_results(unsigned index) {
        _results[index] = 0;
    }

    // Verify if the ratios are what we expect. Because we can't be sure about
    // precise timing issues, we can always be off by some percentage. In simpler
    // tests we really expect it to very low, but in more complex tests, with share
    // changes, for instance, they can accumulate
    //
    // The ratios argument is the ratios towards the first class
    void verify(sstring name, std::vector<unsigned> ratios, unsigned expected_error = 1) {
        SEASTAR_ASSERT(ratios.size() == _results.size());
        auto str = name + ":";
        for (auto i = 0ul; i < _results.size(); ++i) {
            str += format(" r[{:d}] = {:d}", i, _results[i]);
        }
        std::cout << str << std::endl;
        for (auto i = 0ul; i < ratios.size(); ++i) {
            int min_expected = ratios[i] * (_results[0] - expected_error);
            int max_expected = ratios[i] * (_results[0] + expected_error);
            BOOST_CHECK_GE(_results[i], min_expected);
            BOOST_CHECK_LE(_results[i], max_expected);
            BOOST_CHECK_EQUAL(_exceptions[i].size(), 0);
        }
    }

    void verify_f(sstring name, std::vector<float> ratios, float error) {
        SEASTAR_ASSERT(ratios.size() == _results.size());
        auto str = name + ":";
        std::vector<float> adjusted_results;
        adjusted_results.reserve(ratios.size());
        for (auto i = 0ul; i < _results.size(); ++i) {
            str += format(" r[{:d}] = {:d}", i, _results[i]);
            adjusted_results.push_back(_results[i] / ratios[i]);
        }
        std::cout << str << std::endl;
        float average_result = 0.0;
        for (auto ar : adjusted_results) {
            average_result += ar;
        }
        average_result /= adjusted_results.size();
        for (auto ar : adjusted_results) {
            auto dev = std::abs(ar - average_result) / average_result;
            BOOST_CHECK_LE(dev, error);
        }
    }

    void verify_x(sstring name, std::vector<int> values) {
        SEASTAR_ASSERT(values.size() == _results.size());
        auto str = name + ":";
        for (auto i = 0ul; i < _results.size(); ++i) {
            str += format(" r[{:d}] = {:d}", i, _results[i]);
        }
        std::cout << str <<std::endl;
        for (unsigned i = 0; i < _results.size(); i++) {
            BOOST_CHECK_EQUAL(_results[i], values[i]);
        }
    }
};

// Equal ratios. Expected equal results.
SEASTAR_THREAD_TEST_CASE(test_fair_queue_equal_2classes) {
    test_env env;

    auto a = env.register_priority_class(10);
    auto b = env.register_priority_class(10);

    for (int i = 0; i < 100; ++i) {
        env.do_op(a, 1);
        env.do_op(b, 1);
    }

    yield().get();
    // allow half the requests in
    env.tick(10);
    env.verify("equal_2classes", {1, 1});
    env.tick(90);
    env.verify("equal_2classes_more", {1, 1});
}

// Equal results, spread among 4 classes.
SEASTAR_THREAD_TEST_CASE(test_fair_queue_equal_4classes) {
    test_env env;

    auto a = env.register_priority_class(10);
    auto b = env.register_priority_class(10);
    auto c = env.register_priority_class(10);
    auto d = env.register_priority_class(10);

    for (int i = 0; i < 100; ++i) {
        env.do_op(a, 1);
        env.do_op(b, 1);
        env.do_op(c, 1);
        env.do_op(d, 1);
    }
    yield().get();
    // allow half the requests in
    env.tick(200);
    env.verify("equal_4classes", {1, 1, 1, 1});
}

// Class2 twice as powerful. Expected class2 to have 2 x more requests.
SEASTAR_THREAD_TEST_CASE(test_fair_queue_different_shares) {
    test_env env;

    auto a = env.register_priority_class(10);
    auto b = env.register_priority_class(20);

    for (int i = 0; i < 100; ++i) {
        env.do_op(a, 1);
        env.do_op(b, 1);
    }
    yield().get();
    // allow half the requests in
    env.tick(10);
    env.verify("different_shares", {1, 2});
    env.tick(90);
    env.verify("different_shares_more", {1, 2});
}

// Classes equally powerful. But Class1 issues twice as expensive requests. Expected Class2 to have 2 x more requests.
SEASTAR_THREAD_TEST_CASE(test_fair_queue_different_weights) {
    test_env env;

    auto a = env.register_priority_class(10);
    auto b = env.register_priority_class(10);

    for (int i = 0; i < 100; ++i) {
        env.do_op(a, 2);
        env.do_op(b, 1);
    }
    yield().get();
    // allow half the requests in
    env.tick(10);
    env.verify("different_weights", {1, 2});
    env.tick(90);
    env.verify("different_weights_more", {1, 2});
}

// Class2 pushes many requests over. Right after, don't expect Class2 to be able to push anything else.
SEASTAR_THREAD_TEST_CASE(test_fair_queue_dominant_queue) {
    test_env env;

    auto a = env.register_priority_class(10);
    auto b = env.register_priority_class(10);

    for (int i = 0; i < 100; ++i) {
        env.do_op(b, 1);
    }
    yield().get();

    // consume all requests
    env.tick(100);
    // zero statistics.
    env.reset_results(b);
    for (int i = 0; i < 20; ++i) {
        env.do_op(a, 1);
        env.do_op(b, 1);
    }
    // allow half the requests in
    env.tick(20);
    env.verify("dominant_queue", {1, 0});
}

// Class2 pushes many requests at first. Right after, don't expect Class1 to be able to do the same
SEASTAR_THREAD_TEST_CASE(test_fair_queue_forgiving_queue) {
    test_env env;

    // The fair_queue preemption logic allows one class to gain exclusive
    // queue access for at most tau duration. Test queue configures the
    // request rate to be 1/us and tau to be 50us, so after (re-)activation
    // a queue can overrun its peer by at most 50 requests.

    auto a = env.register_priority_class(10);
    auto b = env.register_priority_class(10);

    for (int i = 0; i < 100; ++i) {
        env.do_op(a, 1);
    }
    yield().get();

    // consume all requests
    env.tick(100);
    env.reset_results(a);

    for (int i = 0; i < 100; ++i) {
        env.do_op(a, 1);
        env.do_op(b, 1);
    }
    yield().get();

    // allow half the requests in
    env.tick(100);
    // 50 requests should be passed from b, other 100 should be shared 1:1
    env.verify("forgiving_queue", {1, 3}, 2);
}

SEASTAR_THREAD_TEST_CASE(test_fair_queue_forgiving_group) {
    test_env env;

    auto g = env.register_priority_group(10);
    auto a = env.register_priority_class(10, g);
    auto b = env.register_priority_class(10, g);
    auto c = env.register_priority_class(10);

    auto step = [&env] (std::string name, unsigned pc, unsigned nr, std::vector<int> results) {
        for (unsigned i = 0; i < nr; i++) {
            env.do_op(pc, 1);
        }
        env.tick(100);
        env.verify_x("forgiving_group_" + name, std::move(results));
    };

    auto drain = [&] () {
        env.tick(300);
        env.reset_results(a);
        env.reset_results(b);
        env.reset_results(c);
    };

    // Test one -- activate in top-nesetd-nested order
    {
        // Step-1 -- let the top-level clas work on its own and dispatch 100
        // requests. At this point those 100 requests will be dispatched.
        step("1_top", c, 300, {0, 0, 100});

        // Step-2 -- activate a sub-class and its class group, then dispatch
        // 100 more requests. Queue fogrives 50 requests to the group upon
        // activation, then dispatches remaining 50 in 1:1 proportion between
        // two active classes.
        step("1_nest_1", a, 200, {75, 0, 125});

        // Step-3 -- activate another sub-class, then dispatch 100 more requests.
        // This time queue forgives 50 requests to the sub-class, but not to the
        // group, because it's still active since previous step. After it, the
        // top-level class and a subgroup are dispatched in 1:1 ratio, but since
        // the newly activated sub-class has 50 forgiven requests, only _this_
        // class will be dispatched from in this group, the other sub-class would
        // remain idle during this 100-requests dispatch.
        step("1_nest_2", b, 100, {75, 50, 175});

        // Finally, continue dispatching. At this point no more "forgiven" requests
        // are left and all three classes are dispatched from according to their
        // shares. Given it's {1:1}:2 ratios, the addition should be {25, 25, 50}
        step("1_cont", 0, 0, {100, 75, 225});

        drain();
    }

    // Test two -- activate in nested-outer-nested order
    {
        // Step-1 -- netsted class works on its own and dispatches
        // all the requests it wants.
        step("2_nest_1", a, 300, {100, 0, 0});

        // Step-2 -- top-level class activates and gets 50 fogiven requests.
        // 50 remaining are shared between a and c in 1:1 proportion, just like
        // it was in the top-nest-nest sequence above, no difference here
        step("2_top", c, 200, {125, 0, 75});

        // Step-3 -- another nested class activates and gets 50 forgiven
        // requests. 100 requests are shared between c and {a+b} in 1:1
        // fraction, so c gets 50 and a+b get another 50 which all go to
        // b class for its "forgiven" credit
        step("2_nest_2", b, 100, {125, 50, 125});

        // Remaining requests are shared as in test-1 above: {25, 25, 50}
        step("2_cont", 0, 0, {150, 75, 175});

        drain();
    }

    // Test two -- activate in nested-nested-outer order
    {
        // First two steps don't differ from the previous runs -- first
        // class gets 100 requests, then the 2nd activates and gets 50
        // forgiven and remaining 50 are split evenly.
        step("3_nest_1", a, 300, {100, 0, 0});
        step("3_nest_2", b, 200, {125, 75, 0});

        // Step-3 -- top level class activates and get 50 forgiven
        // requests. After it remaining 50 are divided between it and
        // the a+b group in 1:1 fraction, so class c gets 25 more.
        // Classes in group shared the remaining 25 requests in 1:1 as
        // well, so one class get 12 requests and another one gets 13.
        step("3_top", c, 100, {137, 88, 75});

        // The remaining requests are as well shared in {25, 25, 50}
        // proportion just as before (give c more requests so that it
        // can dispatch its goal).
        step("3_cont", c, 50, {162, 113, 125});

        drain();
    }
}

// Classes push requests and then update swap their shares. In the end, should have executed
// the same number of requests.
SEASTAR_THREAD_TEST_CASE(test_fair_queue_update_shares) {
    test_env env;

    auto a = env.register_priority_class(20);
    auto b = env.register_priority_class(10);

    for (int i = 0; i < 500; ++i) {
        env.do_op(a, 1);
        env.do_op(b, 1);
    }

    yield().get();
    // allow 25% of the requests in
    env.tick(250);
    env.update_shares(a, 10);
    env.update_shares(b, 20);

    yield().get();
    // allow 25% of the requests in
    env.tick(250);
    env.verify("update_shares", {1, 1}, 2);
}

// Classes run for a longer period of time. Balance must be kept over many timer
// periods.
SEASTAR_THREAD_TEST_CASE(test_fair_queue_longer_run) {
    test_env env;

    auto a = env.register_priority_class(10);
    auto b = env.register_priority_class(10);

    for (int i = 0; i < 20000; ++i) {
        env.do_op(a, 1);
        env.do_op(b, 1);
    }
    // In total allow half the requests in, but do it over a
    // long period of time, ticking slowly
    for (int i = 0; i < 1000; ++i) {
        sleep(1ms).get();
        env.tick(2);
    }
    env.verify("longer_run", {1, 1}, 2);
}

// Classes run for a longer period of time. Proportional balance must be kept over many timer
// periods, despite unequal shares..
SEASTAR_THREAD_TEST_CASE(test_fair_queue_longer_run_different_shares) {
    test_env env;

    auto a = env.register_priority_class(10);
    auto b = env.register_priority_class(20);

    for (int i = 0; i < 20000; ++i) {
        env.do_op(a, 1);
        env.do_op(b, 1);
    }

    // In total allow half the requests in, but do it over a
    // long period of time, ticking slowly
    for (int i = 0; i < 1000; ++i) {
        sleep(1ms).get();
        env.tick(3);
    }
    env.verify("longer_run_different_shares", {1, 2}, 2);
}

// Classes run with random shares and random requests weights. Proportional operations expected.
SEASTAR_THREAD_TEST_CASE(test_fair_queue_random_run) {
    test_env env;

    std::default_random_engine& generator = testing::local_random_engine;
    std::uniform_int_distribution<uint32_t> shares(1, 5);
    std::uniform_int_distribution<uint32_t> weights(1, 5);

    struct test_class {
        unsigned shares;
        unsigned weight;
        float expected;
        size_t cls;
    };

    auto add_class = [&] (std::optional<unsigned> group) {
        auto s = shares(generator);
        auto w = weights(generator);
        std::cout << format("Add class with {} shares and {} request weight, in group = {}", s, w, group.has_value()) << std::endl;
        return test_class {
            .shares = s,
            .weight = w,
            .expected = float(s)/float(w),
            .cls = env.register_priority_class(s, group),
        };
    };

    unsigned group_shares = 0;
    std::optional<unsigned> group;
    std::uniform_int_distribution<unsigned> in_group(0, 1);
    if (in_group(generator)) {
        group_shares = shares(generator);
        group = env.register_priority_group(group_shares);
        std::cout << format("Add group with {} shares", group_shares) << std::endl;
    }

    auto a = add_class({});
    auto b = add_class(group);
    auto c = add_class(group);

    if (group) {
        b.expected = float(group_shares) * (float(b.shares) / float(b.shares + c.shares)) / float(b.weight);
        c.expected = float(group_shares) * (float(c.shares) / float(b.shares + c.shares)) / float(c.weight);
    }

    unsigned reqs = 3000;

    // Enough requests for the maximum run (half per queue, + leeway)
    for (uint32_t i = 0; i < reqs; ++i) {
        env.do_op(a.cls, a.weight);
        env.do_op(b.cls, b.weight);
        env.do_op(c.cls, c.weight);
    }

    yield().get();
    // In total allow one-third of the requests in
    env.tick(reqs);

    env.verify_f(format("random_run ({:d} requests)", reqs), {a.expected, b.expected, c.expected}, 0.05);
}

// Exhaustive test for waking-up a tree of classes with requests
SEASTAR_THREAD_TEST_CASE(test_fair_queue_nested_wakeups) {
    test_env env;

    std::vector<unsigned> pcs;
    unsigned g0 = env.register_priority_group(1);
    unsigned g1 = env.register_priority_group(1);
    pcs.push_back(env.register_priority_class(1, {}));
    pcs.push_back(env.register_priority_class(1, {}));
    pcs.push_back(env.register_priority_class(1, g0));
    pcs.push_back(env.register_priority_class(1, g0));
    pcs.push_back(env.register_priority_class(1, g0));
    pcs.push_back(env.register_priority_class(1, g1));
    pcs.push_back(env.register_priority_class(1, g1));

    for (unsigned nr_reqs = 1; nr_reqs < 5; nr_reqs++) {
        std::vector<unsigned> targets(nr_reqs, 0);

        auto next = [&] {
            for (unsigned i = 0; i < targets.size(); i++) {
                if (targets[i] < pcs.size() - 1) {
                    targets[i]++;
                    return true;
                }

                targets[i] = 0;
            }

            return false;
        };

        do {
            for (unsigned i = 0; i < targets.size(); i++) {
                env.do_op(pcs[targets[i]], 1);
            }

            auto res = env.tick(targets.size());
            BOOST_REQUIRE_EQUAL(res, targets.size());
            res = env.tick(1);
            BOOST_REQUIRE_EQUAL(res, 0);
        } while (next());
    }
}
