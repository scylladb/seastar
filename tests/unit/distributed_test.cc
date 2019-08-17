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

#include <seastar/core/app-template.hh>
#include <seastar/core/distributed.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/print.hh>

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
        (void)sleep(std::chrono::milliseconds(100 + 100 * engine().cpu_id())).then([this, ref] {
           check();
        });
    }
    virtual void check() {
        assert(!deleted);
    }
    future<> stop() { return make_ready_future<>(); }
};

thread_local bool async_service::deleted = false;

struct X {
    sstring echo(sstring arg) {
        return arg;
    }
    int cpu_id_squared() const {
        auto id = engine().cpu_id();
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

future<> test_that_each_core_gets_the_arguments() {
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

future<> test_functor_version() {
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

future<> test_constructor_argument_is_passed_to_each_core() {
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

future<> test_map_reduce() {
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

future<> test_async() {
    return do_with_distributed<async_service>([] (distributed<async_service>& x) {
        return x.start().then([&x] {
            return x.invoke_on_all(&async_service::run);
        });
    }).then([] {
        return sleep(std::chrono::milliseconds(100 * (smp::count + 1)));
    });
}

future<> test_invoke_on_others() {
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
                        if (engine().cpu_id() != c) {
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


struct remote_worker {
    unsigned current = 0;
    unsigned max_concurrent_observed = 0;
    future<> do_work() {
        ++current;
        max_concurrent_observed = std::max(current, max_concurrent_observed);
        return sleep(10ms).then([this] {
            max_concurrent_observed = std::max(current, max_concurrent_observed);
            --current;
        });
    }
    future<> do_remote_work(shard_id t, smp_service_group ssg) {
        return smp::submit_to(t,  ssg, [this] {
            return do_work();
        });
    }
};

future<> test_smp_service_groups() {
    return async([] {
        smp_service_group_config ssgc1;
        ssgc1.max_nonlocal_requests = 1;
        auto ssg1 = create_smp_service_group(ssgc1).get0();
        smp_service_group_config ssgc2;
        ssgc2.max_nonlocal_requests = 1000;
        auto ssg2 = create_smp_service_group(ssgc2).get0();
        shard_id other_shard = smp::count - 1;
        remote_worker rm1;
        remote_worker rm2;
        auto bunch1 = parallel_for_each(boost::irange(0, 20), [&] (int ignore) { return rm1.do_remote_work(other_shard, ssg1); });
        auto bunch2 = parallel_for_each(boost::irange(0, 2000), [&] (int ignore) { return rm2.do_remote_work(other_shard, ssg2); });
        bunch1.get();
        bunch2.get();
        if (smp::count > 1) {
            assert(rm1.max_concurrent_observed == 1);
            assert(rm2.max_concurrent_observed >= 1000 / (smp::count - 1) && rm2.max_concurrent_observed <= 1000);
        }
        destroy_smp_service_group(ssg1).get();
        destroy_smp_service_group(ssg2).get();
    });
}

future<> test_smp_service_groups_re_construction() {
    // During development of the feature, we saw a bug where the vector
    // holding the groups did not expand correctly. This test triggers the
    // bug.
    return async([] {
        auto ssg1 = create_smp_service_group({}).get0();
        auto ssg2 = create_smp_service_group({}).get0();
        destroy_smp_service_group(ssg1).get();
        auto ssg3 = create_smp_service_group({}).get0();
        destroy_smp_service_group(ssg2).get();
        destroy_smp_service_group(ssg3).get();
    });
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        return test_that_each_core_gets_the_arguments().then([] {
            return test_functor_version();
        }).then([] {
            return test_constructor_argument_is_passed_to_each_core();
        }).then([] {
            return test_map_reduce();
        }).then([] {
            return test_async();
        }).then([] {
            return test_invoke_on_others();
        }).then([] {
            return test_smp_service_groups();
        }).then([] {
            return test_smp_service_groups_re_construction();
        });
    });
}
