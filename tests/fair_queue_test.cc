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

#include "core/thread.hh"
#include "core/do_with.hh"
#include "test-utils.hh"
#include "core/sstring.hh"
#include "core/reactor.hh"
#include "core/fair_queue.hh"
#include "core/do_with.hh"
#include "core/future-util.hh"
#include "core/sleep.hh"
#include <boost/range/irange.hpp>
#include <random>
#include <chrono>

using namespace std::chrono_literals;

struct test_env {
    fair_queue fq;
    std::vector<int> results;
    std::vector<priority_class_ptr> classes;
    std::vector<future<>> inflight;
    test_env(unsigned capacity) : fq(capacity)
    {}

    size_t register_priority_class(uint32_t shares) {
        results.push_back(0);
        classes.push_back(fq.register_priority_class(shares));
        return classes.size() - 1;
    }
    void do_op(unsigned index, unsigned weight)  {
        auto cl = classes[index];
        auto f = fq.queue(cl, weight, [this, index] {
            results[index]++;
            return sleep(100us);
        });
        inflight.push_back(std::move(f));
    }
    void update_shares(unsigned index, uint32_t shares) {
        auto cl = classes[index];
        fq.update_shares(cl, shares);
    }
    // Verify if the ratios are what we expect. Because we can't be sure about
    // precise timing issues, we can always be off by some percentage. In simpler
    // tests we really expect it to very low, but in more complex tests, with share
    // changes, for instance, they can accumulate
    //
    // The ratios argument is the ratios towards the first class
    future<> verify(sstring name, std::vector<unsigned> ratios, unsigned expected_error = 1) {
        return wait_on_pending().then([name, r = results, ratios = std::move(ratios), this, expected_error] {
            assert(ratios.size() == r.size());
            auto str = name + ":";
            for (auto i = 0ul; i < r.size(); ++i) {
                str += sprint(" r[%ld] = %d", i, r[i]);
            }
            std::cout << str << std::endl;
            for (auto i = 0ul; i < ratios.size(); ++i) {
                int min_expected = ratios[i] * (r[0] - expected_error);
                int max_expected = ratios[i] * (r[0] + expected_error);
                BOOST_REQUIRE(r[i] >= min_expected);
                BOOST_REQUIRE(r[i] <= max_expected);
            }
            for (auto& p: classes) {
                fq.unregister_priority_class(p);
            }
        });
    }
    future<> wait_on_pending() {
        auto curr = make_lw_shared<std::vector<future<>>>();
        curr->swap(inflight);
        return when_all(curr->begin(), curr->end()).discard_result();
    }
};

// Equal ratios. Expected equal results.
SEASTAR_TEST_CASE(test_fair_queue_equal_2classes) {
    auto env = make_lw_shared<test_env>(1);

    auto a = env->register_priority_class(10);
    auto b = env->register_priority_class(10);

    for (int i = 0; i < 100; ++i) {
        env->do_op(a, 1);
        env->do_op(b, 1);
    }
    return sleep(10ms).then([env] {
        return env->verify("equal_2classes", {1, 1});
    }).then([env] {});
}

// Equal results, spread among 4 classes.
SEASTAR_TEST_CASE(test_fair_queue_equal_4classes) {
    auto env = make_lw_shared<test_env>(1);

    auto a = env->register_priority_class(10);
    auto b = env->register_priority_class(10);
    auto c = env->register_priority_class(10);
    auto d = env->register_priority_class(10);

    for (int i = 0; i < 100; ++i) {
        env->do_op(a, 1);
        env->do_op(b, 1);
        env->do_op(c, 1);
        env->do_op(d, 1);
    }
    return sleep(10ms).then([env] {
        return env->verify("equal_4classes", {1, 1, 1, 1});
    }).then([env] {});
}

// Class2 twice as powerful. Expected class2 to have 2 x more requests.
SEASTAR_TEST_CASE(test_fair_queue_different_shares) {
    auto env = make_lw_shared<test_env>(1);

    auto a = env->register_priority_class(10);
    auto b = env->register_priority_class(20);

    for (int i = 0; i < 100; ++i) {
        env->do_op(a, 1);
        env->do_op(b, 1);
    }
    return sleep(10ms).then([env] {
        return env->verify("different_shares", {1, 2});
    }).then([env] {});
}

// Equal ratios, high capacity queue. Should still divide equally.
//
// Note that we sleep less because now more requests will be going through the
// queue.
SEASTAR_TEST_CASE(test_fair_queue_equal_hi_capacity_2classes) {
    auto env = make_lw_shared<test_env>(10);

    auto a = env->register_priority_class(10);
    auto b = env->register_priority_class(10);

    for (int i = 0; i < 100; ++i) {
        env->do_op(a, 1);
        env->do_op(b, 1);
    }
    return sleep(1ms).then([env] {
        return env->verify("hi_capacity_2classes", {1, 1});
    }).then([env] {});

}

// Class2 twice as powerful, queue is high capacity. Still expected class2 to
// have 2 x more requests.
//
// Note that we sleep less because now more requests will be going through the
// queue.
SEASTAR_TEST_CASE(test_fair_queue_different_shares_hi_capacity) {
    auto env = make_lw_shared<test_env>(10);

    auto a = env->register_priority_class(10);
    auto b = env->register_priority_class(20);

    for (int i = 0; i < 100; ++i) {
        env->do_op(a, 1);
        env->do_op(b, 1);
    }
    return sleep(1ms).then([env] {
        return env->verify("different_shares_hi_capacity", {1, 2});
    }).then([env] {});
}

// Classes equally powerful. But Class1 issues twice as expensive requests. Expected Class2 to have 2 x more requests.
SEASTAR_TEST_CASE(test_fair_queue_different_weights) {
    auto env = make_lw_shared<test_env>(1);

    auto a = env->register_priority_class(10);
    auto b = env->register_priority_class(10);

    for (int i = 0; i < 100; ++i) {
        env->do_op(a, 2);
        env->do_op(b, 1);
    }
    return sleep(5ms).then([env] {
        return env->verify("different_weights", {1, 2});
    }).then([env] {});
}

// Class2 pushes many requests over 10ms. In the next msec at least, don't expect Class2 to be able to push anything else.
SEASTAR_TEST_CASE(test_fair_queue_dominant_queue) {
    auto env = make_lw_shared<test_env>(1);

    auto a = env->register_priority_class(10);
    auto b = env->register_priority_class(10);

    for (int i = 0; i < 100; ++i) {
        env->do_op(b, 1);
    }
    return env->wait_on_pending().then([env, a, b] {
        env->results[b] = 0;
        for (int i = 0; i < 20; ++i) {
            env->do_op(a, 1);
            env->do_op(b, 1);
        }
        return sleep(1ms).then([env] {
            return env->verify("dominant_queue", {1, 0});
        });
    }).then([env] {});
}

// Class2 pushes many requests over 10ms. After enough time, this shouldn't matter anymore.
SEASTAR_TEST_CASE(test_fair_queue_forgiving_queue) {
    auto env = make_lw_shared<test_env>(1);

    auto a = env->register_priority_class(10);
    auto b = env->register_priority_class(10);

    for (int i = 0; i < 100; ++i) {
        env->do_op(b, 1);
    }
    return env->wait_on_pending().then([] {
        return sleep(500ms);
    }).then([env, a, b] {
        env->results[b] = 0;
        for (int i = 0; i < 100; ++i) {
            env->do_op(a, 1);
            env->do_op(b, 1);
        }
        return sleep(10ms).then([env] {
            return env->verify("forgiving_queue", {1, 1});
        });
    }).then([env] {});
}

// Classes push requests and then update swap their shares. In the end, should have executed
// the same number of requests.
SEASTAR_TEST_CASE(test_fair_queue_update_shares) {
    auto env = make_lw_shared<test_env>(1);

    auto a = env->register_priority_class(20);
    auto b = env->register_priority_class(10);

    for (int i = 0; i < 500; ++i) {
        env->do_op(a, 1);
        env->do_op(b, 1);
    }
    return sleep(10ms).then([env, a, b] {
       env->update_shares(a, 10);
       env->update_shares(b, 20);
       return sleep(10ms);
    }).then([env] {
       return env->verify("update_shares", {1, 1}, 2);
    }).then([env] {});
}

// Classes run for a longer period of time. Balance must be kept over many timer
// periods.
SEASTAR_TEST_CASE(test_fair_queue_longer_run) {
    auto env = make_lw_shared<test_env>(1);

    auto a = env->register_priority_class(10);
    auto b = env->register_priority_class(10);

    for (int i = 0; i < 20000; ++i) {
        env->do_op(a, 1);
        env->do_op(b, 1);
    }
    return sleep(1s).then([env, a, b] {
       return env->verify("longer_run", {1, 1}, 2);
    }).then([env] {});
}

// Classes run for a longer period of time. Proportional balance must be kept over many timer
// periods, despite unequal shares..
SEASTAR_TEST_CASE(test_fair_queue_longer_run_different_shares) {
    auto env = make_lw_shared<test_env>(1);

    auto a = env->register_priority_class(10);
    auto b = env->register_priority_class(20);

    for (int i = 0; i < 20000; ++i) {
        env->do_op(a, 1);
        env->do_op(b, 1);
    }
    return sleep(1s).then([env, a, b] {
       return env->verify("longer_run_different_shares", {1, 2}, 2);
    }).then([env] {});
}

// Classes run for a random period of time. Equal operations expected.
SEASTAR_TEST_CASE(test_fair_queue_random_run) {
    auto env = make_lw_shared<test_env>(1);

    auto a = env->register_priority_class(1);
    auto b = env->register_priority_class(1);

    auto seed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::default_random_engine generator(seed);
    // multiples of 100usec - which is the approximate length of the request. We will
    // put a minimum of 10. Below that, it is hard to guarantee anything. The maximum is
    // about 50 seconds.
    std::uniform_int_distribution<uint32_t> distribution(10, 500 * 1000);
    auto reqs = distribution(generator);

    // Enough requests for the maximum run (half per queue, + leeway)
    for (uint32_t i = 0; i < (reqs / 2) + 10; ++i) {
        env->do_op(a, 1);
        env->do_op(b, 1);
    }

    return sleep(reqs * 100us).then([env, a, b, reqs] {
        // Accept 5 % error.
        auto expected_error = std::max(1, int(round(reqs * 0.05)));
       return env->verify(sprint("random_run (%d msec)", reqs / 10), {1, 1}, expected_error);
    }).then([env] {});
}
