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

#include <seastar/core/thread.hh>
#include <seastar/core/do_with.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/shared_mutex.hh>
#include <ranges>
#include <stdexcept>

#include "expected_exception.hh"

using namespace seastar;
using namespace std::chrono_literals;

SEASTAR_TEST_CASE(test_semaphore_consume) {
    semaphore sem(0);
    sem.consume(1);
    BOOST_REQUIRE_EQUAL(sem.current(), 0u);
    BOOST_REQUIRE_EQUAL(sem.waiters(), 0u);

    BOOST_REQUIRE_EQUAL(sem.try_wait(0), false);
    auto fut = sem.wait(1);
    BOOST_REQUIRE_EQUAL(fut.available(), false);
    BOOST_REQUIRE_EQUAL(sem.waiters(), 1u);
    sem.signal(2);
    BOOST_REQUIRE_EQUAL(sem.waiters(), 0u);
    return make_ready_future<>();
}

SEASTAR_TEST_CASE(test_semaphore_1) {
    return do_with(std::make_pair(semaphore(0), 0), [] (std::pair<semaphore, int>& x) {
        (void)x.first.wait().then([&x] {
            x.second++;
        });
        x.first.signal();
        return sleep(10ms).then([&x] {
            BOOST_REQUIRE_EQUAL(x.second, 1);
        });
    });
}

SEASTAR_THREAD_TEST_CASE(test_semaphore_2) {
    auto sem = std::make_optional<semaphore>(0);
    int x = 0;
    auto fut = sem->wait().then([&x] {
        x++;
    });
    sleep(10ms).get();
    BOOST_REQUIRE_EQUAL(x, 0);
    sem = std::nullopt;
    BOOST_CHECK_THROW(fut.get(), broken_promise);
}

SEASTAR_TEST_CASE(test_semaphore_timeout_1) {
    return do_with(std::make_pair(semaphore(0), 0), [] (std::pair<semaphore, int>& x) {
        (void)x.first.wait(100ms).then([&x] {
            x.second++;
        });
        (void)sleep(3ms).then([&x] {
            x.first.signal();
        });
        return sleep(200ms).then([&x] {
            BOOST_REQUIRE_EQUAL(x.second, 1);
        });
    });
}

SEASTAR_THREAD_TEST_CASE(test_semaphore_timeout_2) {
    auto sem = semaphore(0);
    int x = 0;
    auto fut1 = sem.wait(3ms).then([&x] {
        x++;
    });
    bool signaled = false;
    auto fut2 = sleep(100ms).then([&sem, &signaled] {
        signaled = true;
        sem.signal();
    });
    sleep(200ms).get();
    fut2.get();
    BOOST_REQUIRE_EQUAL(signaled, true);
    BOOST_CHECK_THROW(fut1.get(), semaphore_timed_out);
    BOOST_REQUIRE_EQUAL(x, 0);
}

SEASTAR_THREAD_TEST_CASE(test_semaphore_mix_1) {
    auto sem = semaphore(0);
    int x = 0;
    auto fut1 = sem.wait(30ms).then([&x] {
        x++;
    });
    auto fut2 = sem.wait().then([&x] {
        x += 10;
    });
    auto fut3 = sleep(100ms).then([&sem] {
        sem.signal();
    });
    sleep(200ms).get();
    fut3.get();
    fut2.get();
    BOOST_CHECK_THROW(fut1.get(), semaphore_timed_out);
    BOOST_REQUIRE_EQUAL(x, 10);
}

SEASTAR_TEST_CASE(test_broken_semaphore) {
    auto sem = make_lw_shared<semaphore>(0);
    struct oops {};
    auto check_result = [sem] (future<> f) {
        try {
            f.get();
            BOOST_FAIL("expecting exception");
        } catch (oops& x) {
            // ok
            return make_ready_future<>();
        } catch (...) {
            BOOST_FAIL("wrong exception seen");
        }
        BOOST_FAIL("unreachable");
        return make_ready_future<>();
    };
    auto ret = sem->wait().then_wrapped(check_result);
    sem->broken(oops());
    return sem->wait().then_wrapped(check_result).then([ret = std::move(ret)] () mutable {
        return std::move(ret);
    });
}

SEASTAR_THREAD_TEST_CASE(test_default_broken_semaphore) {
    struct test_semaphore_exception_factory {
        static semaphore_timed_out timeout() noexcept { return semaphore_timed_out(); }
    };
    auto sem = basic_semaphore<test_semaphore_exception_factory>(0);
    auto fut = sem.wait();
    BOOST_REQUIRE(!fut.available());
    sem.broken();
    BOOST_REQUIRE_THROW(fut.get(), broken_semaphore);
    BOOST_REQUIRE_THROW(sem.wait().get(), broken_semaphore);
}

SEASTAR_THREAD_TEST_CASE(test_non_default_broken_semaphore) {
    struct test_semaphore_exception_factory {
        static semaphore_timed_out timeout() noexcept { return semaphore_timed_out(); }
    };
    auto sem = basic_semaphore<test_semaphore_exception_factory>(0);
    auto fut = sem.wait();
    BOOST_REQUIRE(!fut.available());
    sem.broken(expected_exception());
    BOOST_REQUIRE_THROW(fut.get(), expected_exception);
    BOOST_REQUIRE_THROW(sem.wait().get(), expected_exception);
}

SEASTAR_TEST_CASE(test_shared_mutex_exclusive) {
    return do_with(shared_mutex(), unsigned(0), [] (shared_mutex& sm, unsigned& counter) {
        return parallel_for_each(std::views::iota(0, 10), [&sm, &counter] (int idx) {
            return with_lock(sm, [&counter] {
                BOOST_REQUIRE_EQUAL(counter, 0u);
                ++counter;
                return sleep(10ms).then([&counter] {
                    --counter;
                    BOOST_REQUIRE_EQUAL(counter, 0u);
                });
            });
        });
    });
}

SEASTAR_TEST_CASE(test_shared_mutex_shared) {
    return do_with(shared_mutex(), unsigned(0), [] (shared_mutex& sm, unsigned& counter) {
        auto running_in_parallel = [&sm, &counter] (int instance) {
            return with_shared(sm, [&counter] {
                ++counter;
                return sleep(10ms).then([&counter] {
                    bool was_parallel = counter != 0;
                    --counter;
                    return was_parallel;
                });
            });
        };
        return map_reduce(std::views::iota(0, 100), running_in_parallel, false, std::bit_or<bool>()).then([&counter] (bool result) {
            BOOST_REQUIRE_EQUAL(result, true);
            BOOST_REQUIRE_EQUAL(counter, 0u);
        });
    });
}

SEASTAR_TEST_CASE(test_shared_mutex_mixed) {
    return do_with(shared_mutex(), unsigned(0), [] (shared_mutex& sm, unsigned& counter) {
        auto running_in_parallel = [&sm, &counter] (int instance) {
            return with_shared(sm, [&counter] {
                ++counter;
                return sleep(10ms).then([&counter] {
                    bool was_parallel = counter != 0;
                    --counter;
                    return was_parallel;
                });
            });
        };
        auto running_alone = [&sm, &counter] (int instance) {
            return with_lock(sm, [&counter] {
                BOOST_REQUIRE_EQUAL(counter, 0u);
                ++counter;
                return sleep(10ms).then([&counter] {
                    --counter;
                    BOOST_REQUIRE_EQUAL(counter, 0u);
                    return true;
                });
            });
        };
        auto run = [running_in_parallel, running_alone] (int instance) {
            if (instance % 9 == 0) {
                return running_alone(instance);
            } else {
                return running_in_parallel(instance);
            }
        };
        return map_reduce(std::views::iota(0, 100), run, false, std::bit_or<bool>()).then([&counter] (bool result) {
            BOOST_REQUIRE_EQUAL(result, true);
            BOOST_REQUIRE_EQUAL(counter, 0u);
        });
    });
}


SEASTAR_TEST_CASE(test_with_semaphore) {
    return do_with(semaphore(1), 0, [] (semaphore& sem, int& counter) {
        return with_semaphore(sem, 1, [&counter] {
            ++counter;
        }).then([&counter, &sem] () {
            return with_semaphore(sem, 1, [&counter] {
                ++counter;
                throw 123;
            }).then_wrapped([&counter] (auto&& fut) {
                BOOST_REQUIRE_EQUAL(counter, 2);
                BOOST_REQUIRE(fut.failed());
                fut.ignore_ready_future();
            });
        });
    });
}

SEASTAR_THREAD_TEST_CASE(test_semaphore_units_valid_splitting) {
    auto sm = semaphore(2);
    auto units = get_units(sm, 2, 1min).get();
    {
        BOOST_REQUIRE_EQUAL(units.count(), 2);
        BOOST_REQUIRE_EQUAL(sm.available_units(), 0);
        auto split = units.split(1);
        BOOST_REQUIRE_EQUAL(sm.available_units(), 0);
    }
    BOOST_REQUIRE_EQUAL(sm.available_units(), 1);
}

SEASTAR_THREAD_TEST_CASE(test_semaphore_units_invalid_splitting) {
    auto sm = semaphore(2);
    auto units = get_units(sm, 2, 1min).get();
    BOOST_REQUIRE_EQUAL(sm.available_units(), 0);
    BOOST_REQUIRE_THROW(units.split(10), std::invalid_argument);
    BOOST_REQUIRE_EQUAL(sm.available_units(), 0);
}

SEASTAR_THREAD_TEST_CASE(test_semaphore_units_return_when_destroyed) {
    auto sm = semaphore(3);
  {
    auto units = get_units(sm, 3, 1min).get();
    BOOST_REQUIRE_EQUAL(units.count(), 3);
    BOOST_REQUIRE_EQUAL(sm.available_units(), 0);
    BOOST_REQUIRE_EQUAL(units.return_units(1), 2);
    BOOST_REQUIRE_EQUAL(units.count(), 2);
    BOOST_REQUIRE_EQUAL(sm.available_units(), 1);
  }
    BOOST_REQUIRE_EQUAL(sm.available_units(), 3);
}

SEASTAR_THREAD_TEST_CASE(test_semaphore_units_return_all) {
    auto sm = semaphore(3);
    auto units = get_units(sm, 2, 1min).get();
    BOOST_REQUIRE_EQUAL(sm.available_units(), 1);
    BOOST_REQUIRE_THROW(units.return_units(10), std::invalid_argument);
    BOOST_REQUIRE_EQUAL(sm.available_units(), 1);
    units.return_all();
    BOOST_REQUIRE_EQUAL(units.count(), 0);
    BOOST_REQUIRE_EQUAL(sm.available_units(), 3);
}

SEASTAR_THREAD_TEST_CASE(test_semaphore_try_get_units) {
    constexpr size_t initial_units = 1;
    auto sm = semaphore(initial_units);

    auto opt_units = try_get_units(sm, 1);
    BOOST_REQUIRE(opt_units);

    auto opt_units2 = try_get_units(sm, 1);
    BOOST_REQUIRE(!opt_units2);

    opt_units.reset();
    BOOST_REQUIRE_EQUAL(sm.available_units(), initial_units);

    opt_units = try_get_units(sm, 1);
    BOOST_REQUIRE(opt_units);

    opt_units->return_all();
    BOOST_REQUIRE_EQUAL(opt_units->count(), 0);
    BOOST_REQUIRE_EQUAL(sm.available_units(), initial_units);
}

SEASTAR_THREAD_TEST_CASE(test_semaphore_units_abort) {
    auto sm = semaphore(3);
    auto units = get_units(sm, 3, 1min).get();
    BOOST_REQUIRE_EQUAL(units.count(), 3);

    abort_source as;

    auto f = get_units(sm, 1, as);
    BOOST_REQUIRE(!f.available());

    (void)sleep(1ms).then([&as] {
        as.request_abort();
    });

    BOOST_REQUIRE_THROW(f.get(), semaphore_aborted);
}

SEASTAR_THREAD_TEST_CASE(test_semaphore_units_bool_operator) {
    auto sem = semaphore(2);
    semaphore_units u0;
    BOOST_REQUIRE(!bool(u0));

    u0 = get_units(sem, 2).get();
    BOOST_REQUIRE(bool(u0));

    u0.return_units(1);
    BOOST_REQUIRE(bool(u0));

    auto n = u0.release();
    BOOST_REQUIRE(!bool(u0));
    sem.signal(n);

    u0 = get_units(sem, 2).get();
    BOOST_REQUIRE(bool(u0));
    auto u1 = std::move(u0);
    BOOST_REQUIRE(bool(u1));
    BOOST_REQUIRE(!bool(u0));

    u1.return_all();
    BOOST_REQUIRE(!bool(u1));

    u0 = get_units(sem, 2).get();
    BOOST_REQUIRE(bool(u0));
    u1 = u0.split(1);
    BOOST_REQUIRE(bool(u1));
    BOOST_REQUIRE(bool(u0));

    u0.adopt(std::move(u1));
    BOOST_REQUIRE(!bool(u1));
    BOOST_REQUIRE(bool(u0));
}

SEASTAR_THREAD_TEST_CASE(test_named_semaphore_error) {
    auto sem = make_lw_shared<named_semaphore>(0, named_semaphore_exception_factory{"name_of_the_semaphore"});
    auto check_result = [sem] (future<> f) {
        try {
            f.get();
            BOOST_FAIL("Expecting an exception");
        } catch (broken_named_semaphore& ex) {
            BOOST_REQUIRE_NE(std::string(ex.what()).find("name_of_the_semaphore"), std::string::npos);
        } catch (...) {
            BOOST_FAIL("Expected an instance of broken_named_semaphore with proper semaphore name");
        }
        return make_ready_future<>();
    };
    auto ret = sem->wait().then_wrapped(check_result);
    sem->broken();
    sem->wait().then_wrapped(check_result).then([ret = std::move(ret)] () mutable {
        return std::move(ret);
    }).get();
}

SEASTAR_THREAD_TEST_CASE(test_named_semaphore_timeout) {
    auto sem = make_lw_shared<named_semaphore>(0, named_semaphore_exception_factory{"name_of_the_semaphore"});

    auto f = sem->wait(named_semaphore::clock::now() + 1ms, 1);
    try {
        f.get();
        BOOST_FAIL("Expecting an exception");
    } catch (named_semaphore_timed_out& ex) {
        BOOST_REQUIRE_NE(std::string(ex.what()).find("name_of_the_semaphore"), std::string::npos);
    } catch (...) {
        BOOST_FAIL("Expected an instance of named_semaphore_timed_out with proper semaphore name");
    }
}

SEASTAR_THREAD_TEST_CASE(test_semaphore_abort_after_wait) {
    auto sem = semaphore(0);
    abort_source as;
    int x = 0;
    auto fut1 = sem.wait(as).then([&x] {
        x++;
    });
    as.request_abort();
    sem.signal();
    BOOST_CHECK_THROW(fut1.get(), semaphore_aborted);
    BOOST_REQUIRE_EQUAL(x, 0);
}

SEASTAR_THREAD_TEST_CASE(test_semaphore_abort_with_exception_after_wait) {
    auto sem = semaphore(0);
    abort_source as;
    int x = 0;
    auto fut1 = sem.wait(as).then([&x] {
        x++;
    });
    as.request_abort_ex(expected_exception());
    sem.signal();
    BOOST_CHECK_THROW(fut1.get(), expected_exception);
    BOOST_REQUIRE_EQUAL(x, 0);
}

SEASTAR_THREAD_TEST_CASE(test_semaphore_abort_before_wait) {
    auto sem = semaphore(0);
    abort_source as;
    int x = 0;
    as.request_abort();
    auto fut1 = sem.wait(as).then([&x] {
        x++;
    });
    sem.signal();
    BOOST_CHECK_THROW(fut1.get(), semaphore_aborted);
    BOOST_REQUIRE_EQUAL(x, 0);
}

SEASTAR_THREAD_TEST_CASE(test_reassigned_units_are_returned) {
    auto sem0 = semaphore(1);
    auto sem1 = semaphore(1);
    auto units = get_units(sem0, 1).get();
    auto wait = sem0.wait(1);
    BOOST_REQUIRE(!wait.available());
    units = get_units(sem1, 1).get();
    timer t([] { abort(); });
    t.arm(1s);
    // will hang if units are not returned when reassigned
    wait.get();
    t.cancel();
}

SEASTAR_THREAD_TEST_CASE(test_get_units_after_move) {
    auto sem = std::make_unique<semaphore>([] { return semaphore(0); }());
    auto f = get_units(*sem, 1);
    BOOST_REQUIRE(!f.available());
    sem->signal();
    f.get();
}

SEASTAR_THREAD_TEST_CASE(test_get_units_abort_after_move) {
    auto sem = std::make_unique<semaphore>([] { return semaphore(0); }());
    abort_source as;
    auto f = get_units(*sem, 1, as);
    BOOST_REQUIRE(!f.available());
    as.request_abort();

    BOOST_REQUIRE_THROW(f.get(), abort_requested_exception);
}

SEASTAR_THREAD_TEST_CASE(test_immediate_abort_after_move) {
    auto sem = std::make_unique<semaphore>([] { return semaphore(0); }());
    abort_source as;
    as.request_abort();

    BOOST_REQUIRE_THROW(get_units(*sem, 1, as).get(), abort_requested_exception);
}

SEASTAR_THREAD_TEST_CASE(test_get_units_after_move_assign) {
    auto sem = semaphore(0);
    sem = [] { return semaphore(0); }();
    auto f = get_units(sem, 1);
    BOOST_REQUIRE(!f.available());
    sem.signal();
    f.get();
}

SEASTAR_THREAD_TEST_CASE(test_get_units_abort_after_move_assign) {
    auto sem = semaphore(0);
    sem = [] { return semaphore(0); }();
    abort_source as;
    auto f = get_units(sem, 1, as);
    BOOST_REQUIRE(!f.available());
    as.request_abort();

    BOOST_REQUIRE_THROW(f.get(), abort_requested_exception);
}

SEASTAR_THREAD_TEST_CASE(test_immediate_abort_after_move_assign) {
    auto sem = semaphore(0);
    sem = [] { return semaphore(0); }();
    abort_source as;
    as.request_abort();

    BOOST_REQUIRE_THROW(get_units(sem, 1, as).get(), abort_requested_exception);
}
