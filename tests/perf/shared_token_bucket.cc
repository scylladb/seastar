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
 * Copyright (C) 2023 ScyllaDB Ltd.
 */


#include <random>
#include <seastar/testing/perf_tests.hh>
#include <seastar/testing/random.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/later.hh>
#include <seastar/util/shared_token_bucket.hh>

// The test allows measuring if the shared_token_bucket<> allows the tokens
// consumers to get tokens at the rate the bucket is configured with.
//
// Report example:
//
//     effective rate is 1016575.0t/s, [239668 ... 263522]
//
// The last line "effective rate" is the tokens-per-second rate all workers were
// able to get. It should be equal to the configured rate (context::rate below)
//
// In braces there are minimal and maximum rate of individual shards. These numbers
// should not differ to much from each other, if they do it means that the t.b.
// is not fair

using clock_type = std::chrono::steady_clock;

// The test uses uint64_t tokens with per-second time-measurement and can use
// capped and non-capped token buckets.
using capped_token_bucket_t = internal::shared_token_bucket<uint64_t, std::ratio<1>, internal::capped_release::yes>;
using pure_token_bucket_t = internal::shared_token_bucket<uint64_t, std::ratio<1>, internal::capped_release::no>;

// The test entry point calls map_reduce on the sharded<> workers set and this
// is what each worker produces, so that the entry point could accumulate the
// final result from
struct work_result {
    uint64_t tokens;
    uint64_t released;
};

struct statistics {
    uint64_t total = 0;
    uint64_t min = std::numeric_limits<uint64_t>::max();
    uint64_t max = std::numeric_limits<uint64_t>::min();
};

statistics accumulate(statistics acc, const work_result& val) {
    return statistics {
        .total = acc.total + val.tokens,
        .min = std::min(acc.min, val.tokens),
        .max = std::max(acc.max, val.tokens),
    };
}

// The worker itself. Has a reference on the shared token bucket and tries to
// consume as many tokens as it can without artificial delays. Reports the number
// of tokens grabbed from bucket while running
template <typename TokenBucket>
struct worker : public seastar::peering_sharded_service<worker<TokenBucket>> {
    TokenBucket& tb;

    // Capped bucket requires that t.b. user releases the tokens so that
    // they could be replenished. Respectively, the worker keeps track of
    // total number of grabbed tokens as well as the number of not-yet-released
    // tokens and the total number of released tokens. The "available" number
    // of tokens is used to decide if the worker can release them or not
    uint64_t tokens = 0;
    uint64_t available = 0;
    uint64_t released = 0;

    // Tokens are released in a timer. This mimics default reactor workflow
    // when requests are reaped from the kernel each task-quota -- 0.5ms
    static constexpr auto release_period = std::chrono::microseconds(500);
    const uint64_t release_per_tick = 0;
    clock_type::time_point last_release;
    timer<> release_tokens;

    std::optional<std::pair<int, uint64_t>> head;
    // The IO-scheduler doesn't get more than this number of tokens per tick.
    // The test tries to mimic this behavior
    const uint64_t threshold;

    // The number of tokens to grab at a time. It's a distribution to resemble
    // IO queue that tries to grab different amount of tokens for requests of
    // different sizes and direction
    std::uniform_int_distribution<int> size;

    // Per-tick statistics. Collected, but not reported by default
    struct tick_data {
        std::chrono::microseconds delay;
        std::chrono::microseconds sleep;
        uint64_t defic;
        uint64_t tokens;
        uint64_t total;

        template <typename D1, typename D2>
        tick_data(D1 dur, D2 slp, uint64_t def, uint64_t lt, uint64_t tt) noexcept
                : delay(std::chrono::duration_cast<std::chrono::microseconds>(dur))
                , sleep(std::chrono::duration_cast<std::chrono::microseconds>(slp))
                , defic(def)
                , tokens(lt)
                , total(tt)
        {}
    };
    std::deque<tick_data> ticks;

    worker(TokenBucket& tb_) noexcept
        : tb(tb_)
        , release_per_tick(double(tb.rate()) / smp::count * std::chrono::duration_cast<std::chrono::duration<double>>(release_period).count())
        , last_release(clock_type::now())
        , release_tokens([this] { do_release(); })
        , threshold(tb.limit() / smp::count)
        , size(1, std::min<int>(threshold, 128))
    {
        release_tokens.arm_periodic(std::chrono::duration_cast<std::chrono::microseconds>(release_period));
        fmt::print("{} worker, threshold {}, release-per-tick {}\n", this_shard_id(), threshold, release_per_tick);
    }

    void do_release(uint64_t tokens) {
        available -= tokens;
        released += tokens;
        if constexpr (TokenBucket::is_capped == internal::capped_release::yes) {
            tb.release(tokens);
        }
    }

    void do_release() {
        // Timer can fire later than programmed because of hogs not yielding in a timely
        // manner. If that was an IO queue more requests would have been reaped from the
        // kernel, so do the same here -- scale the number of releasable tokens proportionally
        auto now = clock_type::now();
        auto real_delay = std::chrono::duration_cast<std::chrono::duration<double>>(now - last_release);
        last_release = now;
        uint64_t to_release = real_delay.count() * release_per_tick / std::chrono::duration_cast<std::chrono::duration<double>>(release_period).count();
        do_release(std::min(to_release, available));
    }

    future<work_result> work(std::function<future<>(std::chrono::duration<double> d)> do_sleep) {
        SEASTAR_ASSERT(tokens == 0);
        auto start = clock_type::now();
        // Run for 1 second. The perf suite would restart this method several times
        return do_until([end = start + std::chrono::seconds(1)] { return clock_type::now() >= end; },
            [this, start, do_sleep = std::move(do_sleep)] {
                uint64_t d = 0;
                uint64_t l_tokens = 0;
                int sz;

                while (l_tokens < threshold) {
                    if (head) {
                        tb.replenish(clock_type::now());
                        d = tb.deficiency(head->second);
                        if (d > 0) {
                            break;
                        }
                        sz = head->first;
                        head.reset();
                    } else {
                        sz = size(testing::local_random_engine);
                        auto h = tb.grab(sz);
                        d = tb.deficiency(h);
                        if (d > 0) {
                            head = std::make_pair(sz, h);
                            break;
                        }
                    }
                    tokens += sz;
                    l_tokens += sz;
                    available += sz;
                }

                auto p = tb.duration_for(d);

                ticks.emplace_back(clock_type::now() - start, p, d, l_tokens, tokens);
                if (ticks.size() > 2048) {
                    ticks.pop_front();
                }

                return do_sleep(p);
            }
        ).then([this, start] {
            // Reports:
            //  - shard-id
            //  - total number of tokens and total time taken
            //  - effective speed
            //  - expected speed (token-bucket.rate() / smp::count)
            //  - ticks -- the number of times the worker had change to grab tokens
            //  - the info about tokens releasing
            auto delay = std::chrono::duration_cast<std::chrono::duration<double>>(clock_type::now() - start).count();
            fmt::print("{} {}t/{:.3f}s, speed is {:.1f}t/s goal {:.1f}t/s, {} ticks, released {} (accumulated {})\n", this_shard_id(), tokens, delay,
                    double(tokens) / delay, double(tb.rate()) / smp::count, ticks.size(), released, available);
            do_release(available);
            work_result r {
                .tokens = std::exchange(this->tokens, 0),
                .released = std::exchange(this->released, 0),
            };
            return make_ready_future<work_result>(std::move(r));
        });
    }


    // The below two are how worker waits for the token-bucket deficiency
    // to disappear (i.e. -- when the requested number of tokens are replenished
    //
    // Two options -- poll infinitely or sleep for the estimated (by the
    // bucket method) duration

    future<work_result> work_sleeping() {
        return work([] (std::chrono::duration<double> d) {
            return seastar::sleep(std::chrono::duration_cast<std::chrono::microseconds>(d));
        });
    }

    future<work_result> work_yielding() {
        return work([] (std::chrono::duration<double>) {
            return seastar::yield();
        });
    }

    future<> print_and_clear_ticks() {
        fmt::print("{} {} ticks\n", this_shard_id(), ticks.size());
        std::chrono::microseconds p(0);
        for (auto& td : ticks) {
            fmt::print("  {:8} +{:5} us {:5}/{:5} def {:3} sleep {:5} us\n", td.delay.count(), (td.delay - p).count(), td.tokens, td.total, td.defic, td.sleep.count());
            p = td.delay;
        }
        ticks.clear();
        if (this_shard_id() == smp::count - 1) {
            return make_ready_future<>();
        }

        return this->container().invoke_on(this_shard_id() + 1, &worker::print_and_clear_ticks);
    }
};

// CPU hog that occupies CPU for "busy" duration, then sleeps for "rest" duration
// The actual periods are randomized to be thus "on average"
struct hog {
    std::exponential_distribution<double> busy;
    std::exponential_distribution<double> rest;
    std::optional<future<>> stopped;
    bool keep_going = false;
    uint64_t _iterations = 0;

    template <typename T1, typename T2>
    hog(T1 b, T2 r) noexcept
        : busy(1.0 / std::chrono::duration_cast<std::chrono::duration<double>>(b).count())
        , rest(1.0 / std::chrono::duration_cast<std::chrono::duration<double>>(r).count())
    {}

    void work() {
        SEASTAR_ASSERT(!stopped.has_value());
        keep_going = true;
        stopped = do_until([this] { return !keep_going; },
            [this] {
                auto p = std::chrono::duration<double>(rest(testing::local_random_engine));
                return seastar::sleep(std::chrono::duration_cast<std::chrono::microseconds>(p)).then([this] {
                    _iterations++;
                    auto until = clock_type::now() + std::chrono::duration<double>(busy(testing::local_random_engine));
                    do {
                    } while (clock_type::now() < until && keep_going);
                });
            }
        );
    }

    future<> terminate() {
        SEASTAR_ASSERT(stopped.has_value());
        keep_going = false;
        auto f = std::move(*stopped);
        stopped.reset();
        return f;
    }
};

template <typename TokenBucket>
struct context {
    using worker_t = worker<TokenBucket>;
    TokenBucket tb;
    seastar::sharded<worker_t> w;
    seastar::sharded<hog> h;

    static constexpr uint64_t rate = 1000000;
    static constexpr uint64_t limit = rate / 2000;
    static constexpr uint64_t threshold = 1;

    context() : tb(rate, limit, threshold)
    {
        w.start(std::ref(tb)).get();
        h.start(std::chrono::microseconds(300), std::chrono::microseconds(100)).get();
        fmt::print("Created tb {}t/s (limit {} threshold {})\n", tb.rate(), tb.limit(), tb.threshold());
    }

    ~context() {
        h.stop().get();
        w.stop().get();
    }

    template <typename Fn>
    future<> run_workers(Fn&& fn) {
        auto start = clock_type::now();
        return w.map_reduce0(std::forward<Fn>(fn), statistics{}, accumulate).then([start] (statistics st) {
            auto delay = std::chrono::duration_cast<std::chrono::duration<double>>(clock_type::now() - start).count();
            fmt::print("effective rate is {:.1f}t/s, [{} ... {}]\n", st.total / delay, st.min, st.max);
        });
    }

    future<> test_sleeping() {
        fmt::print("---8<---\n");
        return run_workers(&worker_t::work_sleeping);
    }

    future<> test_yielding() {
        fmt::print("---8<---\n");
        return run_workers(&worker_t::work_yielding);
    }

    future<> test_sleeping_with_hog() {
        fmt::print("---8<---\n");
        return h.invoke_on_all(&hog::work).then([this] {
            return run_workers(&worker_t::work_sleeping).then([this] {
                return h.invoke_on_all(&hog::terminate);
            });
        });
    }
};

struct perf_capped_context : public context<capped_token_bucket_t> {};
struct perf_pure_context : public context<pure_token_bucket_t> {};

// There are 3 tests run over 2 types of buckets:
//
// - poll token bucket for tokens in case of deficiency
// - sleep in case token bucket reports deficiency
// - sleep on deficiency, but run CPU hogs in the background
//
// All tests are run with capped and non-capped (called pure) token buckets

PERF_TEST_F(perf_capped_context, yielding_throughput) { return test_yielding(); }
PERF_TEST_F(perf_capped_context, sleeping_throughput) { return test_sleeping(); }
PERF_TEST_F(perf_capped_context, sleeping_throughput_with_hog) { return test_sleeping_with_hog(); }

PERF_TEST_F(perf_pure_context, yielding_throughput) { return test_yielding(); }
PERF_TEST_F(perf_pure_context, sleeping_throughput) { return test_sleeping(); }
PERF_TEST_F(perf_pure_context, sleeping_throughput_with_hog) { return test_sleeping_with_hog(); }
