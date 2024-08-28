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
#include <seastar/testing/random.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/core/execution_stage.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/defer.hh>

using namespace std::chrono_literals;

using namespace seastar;

SEASTAR_TEST_CASE(test_create_stage_from_lvalue_function_object) {
    return seastar::async([] {
        auto dont_move = [obj = make_shared<int>(53)] { return *obj; };
        auto stage = seastar::make_execution_stage("test", dont_move);
        BOOST_REQUIRE_EQUAL(stage().get(), 53);
        BOOST_REQUIRE_EQUAL(dont_move(), 53);
    });
}

SEASTAR_TEST_CASE(test_create_stage_from_rvalue_function_object) {
    return seastar::async([] {
        auto dont_copy = [obj = std::make_unique<int>(42)] { return *obj; };
        auto stage = seastar::make_execution_stage("test", std::move(dont_copy));
        BOOST_REQUIRE_EQUAL(stage().get(), 42);
    });
}

int func() {
    return 64;
}

SEASTAR_TEST_CASE(test_create_stage_from_function) {
    return seastar::async([] {
        auto stage = seastar::make_execution_stage("test", func);
        BOOST_REQUIRE_EQUAL(stage().get(), 64);
    });
}

template<typename Function, typename Verify>
void test_simple_execution_stage(Function&& func, Verify&& verify) {
    auto stage = seastar::make_execution_stage("test", std::forward<Function>(func));

    std::vector<int> vs;
    std::default_random_engine& gen = testing::local_random_engine;
    std::uniform_int_distribution<> dist(0, 100'000);
    std::generate_n(std::back_inserter(vs), 1'000, [&] { return dist(gen); });

    std::vector<future<int>> fs;
    for (auto v : vs) {
        fs.emplace_back(stage(v));
    }

    for (auto i = 0u; i < fs.size(); i++) {
        verify(vs[i], std::move(fs[i]));
    }
}

SEASTAR_TEST_CASE(test_simple_stage_returning_int) {
    return seastar::async([] {
        test_simple_execution_stage([] (int x) {
            if (x % 2) {
                return x * 2;
            } else {
                throw x;
            }
        }, [] (int original, future<int> result) {
            if (original % 2) {
                BOOST_REQUIRE_EQUAL(original * 2, result.get());
            } else {
                BOOST_REQUIRE_EXCEPTION(result.get(), int, [&] (int v) { return original == v; });
            }
        });
    });
}

SEASTAR_TEST_CASE(test_simple_stage_returning_future_int) {
    return seastar::async([] {
        test_simple_execution_stage([] (int x) {
            if (x % 2) {
                return make_ready_future<int>(x * 2);
            } else {
                return make_exception_future<int>(x);
            }
        }, [] (int original, future<int> result) {
            if (original % 2) {
                BOOST_REQUIRE_EQUAL(original * 2, result.get());
            } else {
                BOOST_REQUIRE_EXCEPTION(result.get(), int, [&] (int v) { return original == v; });
            }
        });
    });
}

template<typename T>
void test_execution_stage_avoids_copy() {
    auto stage = seastar::make_execution_stage("test", [] (T obj) {
        return std::move(obj);
    });

    auto f = stage(T());
    T obj = f.get();
    (void)obj;
}

SEASTAR_TEST_CASE(test_stage_moves_when_cannot_copy) {
    return seastar::async([] {
        struct noncopyable_but_movable {
            noncopyable_but_movable() = default;
            noncopyable_but_movable(const noncopyable_but_movable&) = delete;
            noncopyable_but_movable(noncopyable_but_movable&&) = default;
        };

        test_execution_stage_avoids_copy<noncopyable_but_movable>();
    });
}

SEASTAR_TEST_CASE(test_stage_prefers_move_to_copy) {
    return seastar::async([] {
        struct copyable_and_movable {
            copyable_and_movable() = default;
            copyable_and_movable(const copyable_and_movable&) {
                BOOST_FAIL("should not copy");
            }
            copyable_and_movable(copyable_and_movable&&) = default;
        };

        test_execution_stage_avoids_copy<copyable_and_movable>();
    });
}

SEASTAR_TEST_CASE(test_rref_decays_to_value) {
    return seastar::async([] {
        auto stage = seastar::make_execution_stage("test", [] (std::vector<int>&& vec) {
            return vec.size();
        });

        std::vector<int> tmp;
        std::vector<future<size_t>> fs;
        for (auto i = 0; i < 100; i++) {
            tmp.resize(i);
            fs.emplace_back(stage(std::move(tmp)));
            tmp = std::vector<int>();
        }

        for (size_t i = 0; i < 100; i++) {
            BOOST_REQUIRE_EQUAL(fs[i].get(), i);
        }
    });
}

SEASTAR_TEST_CASE(test_lref_does_not_decay) {
    return seastar::async([] {
        auto stage = seastar::make_execution_stage("test", [] (int& v) {
            v++;
        });

        int value = 0;
        std::vector<future<>> fs;
        for (auto i = 0; i < 100; i++) {
            //fs.emplace_back(stage(value)); // should fail to compile
            fs.emplace_back(stage(seastar::ref(value)));
        }

        for (auto&& f : fs) {
            f.get();
        }
        BOOST_REQUIRE_EQUAL(value, 100);
    });
}

SEASTAR_TEST_CASE(test_explicit_reference_wrapper_is_not_unwrapped) {
    return seastar::async([] {
        auto stage = seastar::make_execution_stage("test", [] (seastar::reference_wrapper<int> v) {
            v.get()++;
        });

        int value = 0;
        std::vector<future<>> fs;
        for (auto i = 0; i < 100; i++) {
            //fs.emplace_back(stage(value)); // should fail to compile
            fs.emplace_back(stage(seastar::ref(value)));
        }

        for (auto&& f : fs) {
            f.get();
        }
        BOOST_REQUIRE_EQUAL(value, 100);
    });
}

SEASTAR_TEST_CASE(test_function_is_class_member) {
    return seastar::async([] {
        struct foo {
            int value = -1;
            int member(int x) {
                return std::exchange(value, x);
            }
        };

        auto stage = seastar::make_execution_stage("test", &foo::member);

        foo object;
        std::vector<future<int>> fs;
        for (auto i = 0; i < 100; i++) {
            fs.emplace_back(stage(&object, i));
        }

        for (auto i = 0; i < 100; i++) {
            BOOST_REQUIRE_EQUAL(fs[i].get(), i - 1);
        }
        BOOST_REQUIRE_EQUAL(object.value, 99);
    });
}

SEASTAR_TEST_CASE(test_function_is_const_class_member) {
    return seastar::async([] {
        struct foo {
            int value = 999;
            int member() const {
                return value;
            }
        };
        auto stage = seastar::make_execution_stage("test", &foo::member);

        const foo object;
        BOOST_REQUIRE_EQUAL(stage(&object).get(), 999);
    });
}

SEASTAR_TEST_CASE(test_stage_stats) {
    return seastar::async([] {
        auto stage = seastar::make_execution_stage("test", [] { });

        BOOST_REQUIRE_EQUAL(stage.get_stats().function_calls_enqueued, 0u);
        BOOST_REQUIRE_EQUAL(stage.get_stats().function_calls_executed, 0u);

        auto fs = std::vector<future<>>();
        static constexpr auto call_count = 53u;
        for (auto i = 0u; i < call_count; i++) {
            fs.emplace_back(stage());
        }

        BOOST_REQUIRE_EQUAL(stage.get_stats().function_calls_enqueued, call_count);

        for (auto i = 0u; i < call_count; i++) {
            fs[i].get();
            BOOST_REQUIRE_GE(stage.get_stats().tasks_scheduled, 1u);
            BOOST_REQUIRE_GE(stage.get_stats().function_calls_executed, i);
        }
        BOOST_REQUIRE_EQUAL(stage.get_stats().function_calls_executed, call_count);
    });
}

SEASTAR_TEST_CASE(test_unique_stage_names_are_enforced) {
    return seastar::async([] {
        {
            auto stage = seastar::make_execution_stage("test", [] {});
            BOOST_REQUIRE_THROW(seastar::make_execution_stage("test", [] {}), std::invalid_argument);
            stage().get();
        }

        auto stage = seastar::make_execution_stage("test", [] {});
        stage().get();
    });
}

SEASTAR_THREAD_TEST_CASE(test_inheriting_concrete_execution_stage) {
    auto sg1 = seastar::create_scheduling_group("sg1", 300).get();
    auto ksg1 = seastar::defer([&] () noexcept { seastar::destroy_scheduling_group(sg1).get(); });
    auto sg2 = seastar::create_scheduling_group("sg2", 100).get();
    auto ksg2 = seastar::defer([&] () noexcept { seastar::destroy_scheduling_group(sg2).get(); });
    auto check_sg = [] (seastar::scheduling_group sg) {
        BOOST_REQUIRE(seastar::current_scheduling_group() == sg);
    };
    auto es = seastar::inheriting_concrete_execution_stage<void, seastar::scheduling_group>("stage", check_sg);
    auto make_attr = [] (scheduling_group sg) {
        seastar::thread_attributes a;
        a.sched_group = sg;
        return a;
    };
    bool done = false;
    auto make_test_thread = [&] (scheduling_group sg) {
        return seastar::thread(make_attr(sg), [&, sg] {
            while (!done) {
                es(sg).get(); // will check if executed with same sg
            };
        });
    };
    auto th1 = make_test_thread(sg1);
    auto th2 = make_test_thread(sg2);
    seastar::sleep(10ms).get();
    done = true;
    th1.join().get();
    th2.join().get();
}

struct a_struct {};

SEASTAR_THREAD_TEST_CASE(test_inheriting_concrete_execution_stage_reference_parameters) {
    // mostly a compile test, but take the opportunity to test that passing
    // by reference preserves the address
    auto check_ref = [] (a_struct& ref, a_struct* ptr) {
        BOOST_REQUIRE_EQUAL(&ref, ptr);
    };
    auto es = seastar::inheriting_concrete_execution_stage<void, a_struct&, a_struct*>("stage", check_ref);
    a_struct obj;
    es(seastar::ref(obj), &obj).get();
}
