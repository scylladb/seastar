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
#include <seastar/core/reactor.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/shared_mutex.hh>
#include <boost/range/irange.hpp>

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

SEASTAR_TEST_CASE(test_semaphore_2) {
    return do_with(std::make_pair(semaphore(0), 0), [] (std::pair<semaphore, int>& x) {
        (void)x.first.wait().then([&x] {
            x.second++;
        });
        return sleep(10ms).then([&x] {
            BOOST_REQUIRE_EQUAL(x.second, 0);
        });
    });
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

SEASTAR_TEST_CASE(test_semaphore_timeout_2) {
    return do_with(std::make_pair(semaphore(0), 0), [] (std::pair<semaphore, int>& x) {
        (void)x.first.wait(3ms).then([&x] {
            x.second++;
        });
        (void)sleep(100ms).then([&x] {
            x.first.signal();
        });
        return sleep(200ms).then([&x] {
            BOOST_REQUIRE_EQUAL(x.second, 0);
        });
    });
}

SEASTAR_TEST_CASE(test_semaphore_mix_1) {
    return do_with(std::make_pair(semaphore(0), 0), [] (std::pair<semaphore, int>& x) {
        (void)x.first.wait(30ms).then([&x] {
            x.second++;
        });
        (void)x.first.wait().then([&x] {
            x.second = 10;
        });
        (void)sleep(100ms).then([&x] {
            x.first.signal();
        });
        return sleep(200ms).then([&x] {
            BOOST_REQUIRE_EQUAL(x.second, 10);
        });
    });
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

SEASTAR_TEST_CASE(test_shared_mutex_exclusive) {
    return do_with(shared_mutex(), unsigned(0), [] (shared_mutex& sm, unsigned& counter) {
        return parallel_for_each(boost::irange(0, 10), [&sm, &counter] (int idx) {
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
        return map_reduce(boost::irange(0, 100), running_in_parallel, false, std::bit_or<bool>()).then([&counter] (bool result) {
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
        return map_reduce(boost::irange(0, 100), run, false, std::bit_or<bool>()).then([&counter] (bool result) {
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

SEASTAR_THREAD_TEST_CASE(test_semaphore_units_splitting) {
    auto sm = semaphore(2);
    auto units = get_units(sm, 2, 1min).get0();
    {
        BOOST_REQUIRE_EQUAL(sm.available_units(), 0);
        auto split = units.split(1);
        BOOST_REQUIRE_EQUAL(sm.available_units(), 0);
    }
    BOOST_REQUIRE_EQUAL(sm.available_units(), 1);
    units.~semaphore_units();
    units = get_units(sm, 2, 1min).get0();
    BOOST_REQUIRE_EQUAL(sm.available_units(), 0);
    BOOST_REQUIRE_THROW(units.split(10), std::invalid_argument);
    BOOST_REQUIRE_EQUAL(sm.available_units(), 0);
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
