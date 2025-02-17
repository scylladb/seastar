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
#include <seastar/core/loop.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/print.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/closeable.hh>
#include <seastar/util/later.hh>
#include <mutex>
#include <ranges>

using namespace seastar;
using namespace std::chrono_literals;

struct async_service : public seastar::async_sharded_service<async_service> {
    thread_local static bool deleted;
    ~async_service() {
        deleted = true;
    }
    void run() {
        auto ref = shared_from_this();
        // Wait a while and check.
        (void)sleep(std::chrono::milliseconds(100 + 100 * this_shard_id())).then([this, ref] {
           check();
        });
    }
    virtual void check() {
        SEASTAR_ASSERT(!deleted);
    }
    future<> stop() { return make_ready_future<>(); }
};

thread_local bool async_service::deleted = false;

struct X {
    sstring echo(sstring arg) {
        return arg;
    }
    int cpu_id_squared() const {
        auto id = this_shard_id();
        return id * id;
    }
    future<> stop() { return make_ready_future<>(); }
};

template <typename T, typename Func>
future<> do_with_distributed(Func&& func) {
    auto x = make_shared<distributed<T>>();
    return func(*x).finally([x] {
        return x->stop();
    }).finally([x]{});
}

SEASTAR_TEST_CASE(test_that_each_core_gets_the_arguments) {
    return do_with_distributed<X>([] (auto& x) {
        return x.start().then([&x] {
            return x.map_reduce([] (sstring msg){
                if (msg != "hello") {
                    throw std::runtime_error("wrong message");
                }
            }, &X::echo, sstring("hello"));
        });
    });
}

SEASTAR_TEST_CASE(test_functor_version) {
    return do_with_distributed<X>([] (auto& x) {
        return x.start().then([&x] {
            return x.map_reduce([] (sstring msg){
                if (msg != "hello") {
                    throw std::runtime_error("wrong message");
                }
            }, [] (X& x) { return x.echo("hello"); });
        });
    });
}

struct Y {
    sstring s;
    Y(sstring s) : s(std::move(s)) {}
    future<> stop() { return make_ready_future<>(); }
};

SEASTAR_TEST_CASE(test_constructor_argument_is_passed_to_each_core) {
    return do_with_distributed<Y>([] (auto& y) {
        return y.start(sstring("hello")).then([&y] {
            return y.invoke_on_all([] (Y& y) {
                if (y.s != "hello") {
                    throw std::runtime_error(format("expected message mismatch, is \"%s\"", y.s));
                }
            });
        });
    });
}

SEASTAR_TEST_CASE(test_map_reduce) {
    return do_with_distributed<X>([] (distributed<X>& x) {
        return x.start().then([&x] {
            return x.map_reduce0(std::mem_fn(&X::cpu_id_squared),
                                 0,
                                 std::plus<int>()).then([] (int result) {
                int n = smp::count - 1;
                if (result != (n * (n + 1) * (2*n + 1)) / 6) {
                    throw std::runtime_error("map_reduce failed");
                }
            });
        });
    });
}

SEASTAR_TEST_CASE(test_map_reduce_lifetime) {
    struct map {
        bool destroyed = false;
        map() = default;
        map(const map&) = default;
        ~map() {
            destroyed = true;
        }
        auto operator()(const X& x) {
            return yield().then([this, &x] {
                BOOST_REQUIRE(!destroyed);
                return x.cpu_id_squared();
            });
        }
    };
    struct reduce {
        long& res;
        bool destroyed = false;
        reduce(long& result)
            : res{result} {}
        reduce(const reduce&) = default;
        ~reduce() {
            destroyed = true;
        }
        auto operator()(int x) {
            return yield().then([this, x] {
                BOOST_REQUIRE(!destroyed);
                res += x;
            });
        }
    };
    return do_with_distributed<X>([] (distributed<X>& x) {
        return x.start().then([&x] {
            return do_with(0L, [&x] (auto& result) {
                return x.map_reduce(reduce{result}, map{}).then([&result] {
                    long n = smp::count - 1;
                    long expected = (n * (n + 1) * (2*n + 1)) / 6;
                    BOOST_REQUIRE_EQUAL(result, expected);
                });
            });
        });
    });
}

SEASTAR_TEST_CASE(test_map_reduce0_lifetime) {
    struct map {
        bool destroyed = false;
        map() = default;
        map(const map&) = default;
        ~map() {
            destroyed = true;
        }
        auto operator()(const X& x) const {
            return yield().then([this, &x] {
                BOOST_REQUIRE(!destroyed);
                return x.cpu_id_squared();
            });
        }
    };
    struct reduce {
        bool destroyed = false;
        reduce() = default;
        reduce(const reduce&) = default;
        ~reduce() {
            destroyed = true;
        }
        auto operator()(long res, int x) {
            BOOST_REQUIRE(!destroyed);
            return res + x;
        }
    };
    return do_with_distributed<X>([] (distributed<X>& x) {
        return x.start().then([&x] {
            return x.map_reduce0(map{}, 0L, reduce{}).then([] (long result) {
                long n = smp::count - 1;
                long expected = (n * (n + 1) * (2*n + 1)) / 6;
                BOOST_REQUIRE_EQUAL(result, expected);
            });
        });
    });
}

SEASTAR_TEST_CASE(test_map_lifetime) {
    struct map {
        bool destroyed = false;
        map() = default;
        map(const map&) = default;
        ~map() {
            destroyed = true;
        }
        auto operator()(const X& x) const {
            return yield().then([this, &x] {
                BOOST_REQUIRE(!destroyed);
                return x.cpu_id_squared();
            });
        }
    };
    return do_with_distributed<X>([] (distributed<X>& x) {
        return x.start().then([&x] {
            return x.map(map{}).then([] (std::vector<int> result) {
                BOOST_REQUIRE_EQUAL(result.size(), smp::count);
                for (size_t i = 0; i < (size_t)smp::count; i++) {
                    BOOST_REQUIRE_EQUAL(result[i], i * i);
                }
            });
        });
    });
}

SEASTAR_TEST_CASE(test_async) {
    return do_with_distributed<async_service>([] (distributed<async_service>& x) {
        return x.start().then([&x] {
            return x.invoke_on_all(&async_service::run);
        });
    }).then([] {
        return sleep(std::chrono::milliseconds(100 * (smp::count + 1)));
    });
}

SEASTAR_TEST_CASE(test_invoke_on_others) {
    return seastar::async([] {
        struct my_service {
            int counter = 0;
            void up() { ++counter; }
            future<> stop() { return make_ready_future<>(); }
        };
        for (unsigned c = 0; c < smp::count; ++c) {
            smp::submit_to(c, [c] {
                return seastar::async([c] {
                    sharded<my_service> s;
                    s.start().get();
                    s.invoke_on_others([](auto& s) { s.up(); }).get();
                    if (s.local().counter != 0) {
                        throw std::runtime_error("local modified");
                    }
                    s.invoke_on_all([c](auto& remote) {
                        if (this_shard_id() != c) {
                            if (remote.counter != 1) {
                                throw std::runtime_error("remote not modified");
                            }
                        }
                    }).get();
                    s.stop().get();
                });
            }).get();
        }
    });
}

SEASTAR_TEST_CASE(test_smp_invoke_on_others) {
    return seastar::async([] {
        std::vector<std::vector<int>> calls;
        calls.reserve(smp::count);
        for (unsigned i = 0; i < smp::count; i++) {
            auto& sv = calls.emplace_back();
            sv.reserve(smp::count);
        }

        smp::invoke_on_all([&calls] {
            return smp::invoke_on_others([&calls, from = this_shard_id()] {
                calls[this_shard_id()].emplace_back(from);
            });
        }).get();

        for (unsigned i = 0; i < smp::count; i++) {
            BOOST_REQUIRE_EQUAL(calls[i].size(), smp::count - 1);
            for (unsigned f = 0; f < smp::count; f++) {
                auto r = std::find(calls[i].begin(), calls[i].end(), f);
                BOOST_REQUIRE_EQUAL(r == calls[i].end(), i == f);
            }
        }
    });
}

struct remote_worker {
    unsigned current = 0;
    unsigned max_concurrent_observed = 0;
    unsigned expected_max;
    semaphore sem{0};
    remote_worker(unsigned expected_max) : expected_max(expected_max) {
    }
    future<> do_work() {
        ++current;
        max_concurrent_observed = std::max(current, max_concurrent_observed);
        if (max_concurrent_observed >= expected_max && sem.current() == 0) {
            sem.signal(semaphore::max_counter());
        }
        return sem.wait().then([this] {
            // Sleep a bit to check if the concurrency goes over the max
            return sleep(100ms).then([this] {
                max_concurrent_observed = std::max(current, max_concurrent_observed);
                --current;
            });
        });
    }
    future<> do_remote_work(shard_id t, smp_service_group ssg) {
        return smp::submit_to(t,  ssg, [this] {
            return do_work();
        });
    }
};

SEASTAR_TEST_CASE(test_smp_service_groups) {
    return async([] {
        smp_service_group_config ssgc1;
        ssgc1.max_nonlocal_requests = 1;
        auto ssg1 = create_smp_service_group(ssgc1).get();
        smp_service_group_config ssgc2;
        ssgc2.max_nonlocal_requests = 1000;
        auto ssg2 = create_smp_service_group(ssgc2).get();
        shard_id other_shard = smp::count - 1;
        remote_worker rm1(1);
        remote_worker rm2(1000);
        auto bunch1 = parallel_for_each(std::views::iota(0, 20), [&] (int ignore) { return rm1.do_remote_work(other_shard, ssg1); });
        auto bunch2 = parallel_for_each(std::views::iota(0, 2000), [&] (int ignore) { return rm2.do_remote_work(other_shard, ssg2); });
        bunch1.get();
        bunch2.get();
        if (smp::count > 1) {
            SEASTAR_ASSERT(rm1.max_concurrent_observed == 1);
            SEASTAR_ASSERT(rm2.max_concurrent_observed == 1000);
        }
        destroy_smp_service_group(ssg1).get();
        destroy_smp_service_group(ssg2).get();
    });
}

SEASTAR_TEST_CASE(test_smp_service_groups_re_construction) {
    // During development of the feature, we saw a bug where the vector
    // holding the groups did not expand correctly. This test triggers the
    // bug.
    return async([] {
        auto ssg1 = create_smp_service_group({}).get();
        auto ssg2 = create_smp_service_group({}).get();
        destroy_smp_service_group(ssg1).get();
        auto ssg3 = create_smp_service_group({}).get();
        destroy_smp_service_group(ssg2).get();
        destroy_smp_service_group(ssg3).get();
    });
}

SEASTAR_TEST_CASE(test_smp_timeout) {
    return async([] {
        smp_service_group_config ssgc1;
        ssgc1.max_nonlocal_requests = 1;
        auto ssg1 = create_smp_service_group(ssgc1).get();

        auto _ = defer([ssg1] () noexcept {
            destroy_smp_service_group(ssg1).get();
        });

        const shard_id other_shard = smp::count - 1;

        // Ugly but beats using sleeps.
        std::mutex mut;
        std::unique_lock<std::mutex> lk(mut);

        // Submitted to the remote shard.
        auto fut1 = smp::submit_to(other_shard, ssg1, [&mut] {
            std::cout << "Running request no. 1" << std::endl;
            std::unique_lock<std::mutex> lk(mut);
            std::cout << "Request no. 1 done" << std::endl;
        });
        // Consume the only unit from the semaphore.
        auto fut2 = smp::submit_to(other_shard, ssg1, [] {
            std::cout << "Running request no. 2 - done" << std::endl;
        });

        auto fut_timedout = smp::submit_to(other_shard, smp_submit_to_options(ssg1, smp_timeout_clock::now() + 10ms), [] {
            std::cout << "Running timed-out request - done" << std::endl;
        });

        {
            auto notify = defer([lk = std::move(lk)] () noexcept { });

            try {
                fut_timedout.get();
                throw std::runtime_error("smp::submit_to() didn't timeout as expected");
            } catch (semaphore_timed_out& e) {
                std::cout << "Expected timeout received: " << e.what() << std::endl;
            } catch (...) {
                std::throw_with_nested(std::runtime_error("smp::submit_to() failed with unexpected exception"));
            }
        }

        fut1.get();
        fut2.get();
    });
}

SEASTAR_THREAD_TEST_CASE(test_sharded_parameter) {
    struct dependency {
        unsigned val = this_shard_id() * 7;
    };
    struct some_service {
        bool ok = false;
        some_service(unsigned non_shard_dependent, unsigned shard_dependent, dependency& dep, unsigned shard_dependent_2) {
            ok =
                    non_shard_dependent == 43
                    && shard_dependent == this_shard_id() * 3
                    && dep.val == this_shard_id() * 7
                    && shard_dependent_2 == -dep.val;
        }
    };
    sharded<dependency> s_dep;
    s_dep.start().get();
    auto undo1 = deferred_stop(s_dep);

    sharded<some_service> s_service;
    s_service.start(
            43, // should be copied verbatim
            sharded_parameter([] { return this_shard_id() * 3; }),
            std::ref(s_dep),
            sharded_parameter([] (dependency& d) { return -d.val; }, std::ref(s_dep))
            ).get();
    auto undo2 = deferred_stop(s_service);

    auto all_ok = s_service.map_reduce0(std::mem_fn(&some_service::ok), true, std::multiplies<>()).get();
    BOOST_REQUIRE(all_ok);
}
