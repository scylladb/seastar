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
 * Copyright (C) 2020 ScyllaDB.
 */

#include <chrono>
#include <exception>
#include <ranges>
#include <stdexcept>

#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/rwlock.hh>
#include <seastar/core/shared_mutex.hh>
#include <seastar/util/alloc_failure_injector.hh>

using namespace seastar;
using namespace std::chrono_literals;

SEASTAR_THREAD_TEST_CASE(test_rwlock) {
    rwlock l;

    l.for_write().lock().get();
    BOOST_REQUIRE(!l.try_write_lock());
    BOOST_REQUIRE(!l.try_read_lock());
    l.for_write().unlock();

    l.for_read().lock().get();
    BOOST_REQUIRE(!l.try_write_lock());
    BOOST_REQUIRE(l.try_read_lock());
    l.for_read().lock().get();
    l.for_read().unlock();
    l.for_read().unlock();
    l.for_read().unlock();

    BOOST_REQUIRE(l.try_write_lock());
    l.for_write().unlock();
}

SEASTAR_TEST_CASE(test_with_lock_mutable) {
    return do_with(rwlock(), [](rwlock& l) {
        return with_lock(l.for_read(), [p = std::make_unique<int>(42)] () mutable {});
    });
}

SEASTAR_TEST_CASE(test_rwlock_exclusive) {
    return do_with(rwlock(), unsigned(0), [] (rwlock& l, unsigned& counter) {
        return parallel_for_each(std::views::iota(0, 10), [&l, &counter] (int idx) {
            return with_lock(l.for_write(), [&counter] {
                BOOST_REQUIRE_EQUAL(counter, 0u);
                ++counter;
                return sleep(1ms).then([&counter] {
                    --counter;
                    BOOST_REQUIRE_EQUAL(counter, 0u);
                });
            });
        });
    });
}

SEASTAR_TEST_CASE(test_rwlock_shared) {
    return do_with(rwlock(), unsigned(0), unsigned(0), [] (rwlock& l, unsigned& counter, unsigned& max) {
        return parallel_for_each(std::views::iota(0, 10), [&l, &counter, &max] (int idx) {
            return with_lock(l.for_read(), [&counter, &max] {
                ++counter;
                max = std::max(max, counter);
                return sleep(1ms).then([&counter] {
                    --counter;
                });
            });
        }).finally([&counter, &max] {
            BOOST_REQUIRE_EQUAL(counter, 0u);
            BOOST_REQUIRE_NE(max, 0u);
        });
    });
}

SEASTAR_THREAD_TEST_CASE(test_rwlock_failed_func) {
    rwlock l;

    // verify that the rwlock is unlocked when func fails
    future<> fut = with_lock(l.for_read(), [] {
        throw std::runtime_error("injected");
    });
    BOOST_REQUIRE_THROW(fut.get(), std::runtime_error);

    fut = with_lock(l.for_write(), [] {
        throw std::runtime_error("injected");
    });
    BOOST_REQUIRE_THROW(fut.get(), std::runtime_error);

    BOOST_REQUIRE(l.try_write_lock());
    l.for_write().unlock();
}

SEASTAR_THREAD_TEST_CASE(test_rwlock_abort) {
    rwlock l;

    l.write_lock().get();

    {
        abort_source as;
        auto f = l.write_lock(as);
        BOOST_REQUIRE(!f.available());

        (void)sleep(1ms).then([&as] {
            as.request_abort();
        });

        BOOST_REQUIRE_THROW(f.get(), semaphore_aborted);
    }

    {
        abort_source as;
        auto f = l.read_lock(as);
        BOOST_REQUIRE(!f.available());

        (void)sleep(1ms).then([&as] {
            as.request_abort();
        });

        BOOST_REQUIRE_THROW(f.get(), semaphore_aborted);
    }
}

SEASTAR_THREAD_TEST_CASE(test_rwlock_hold_abort) {
    rwlock l;

    auto wh = l.hold_write_lock().get();

    {
        abort_source as;
        auto f = l.hold_write_lock(as);
        BOOST_REQUIRE(!f.available());

        (void)sleep(1ms).then([&as] {
            as.request_abort();
        });

        BOOST_REQUIRE_THROW(f.get(), semaphore_aborted);
    }

    {
        abort_source as;
        auto f = l.hold_read_lock(as);
        BOOST_REQUIRE(!f.available());

        (void)sleep(1ms).then([&as] {
            as.request_abort();
        });

        BOOST_REQUIRE_THROW(f.get(), semaphore_aborted);
    }
}

SEASTAR_THREAD_TEST_CASE(test_rwlock_hold) {
    rwlock l;

    auto rl = l.hold_read_lock().get();

    auto opt_rl = l.try_hold_read_lock();
    BOOST_REQUIRE(opt_rl.has_value());
    BOOST_REQUIRE(!l.try_hold_write_lock());

    rl.return_all();
    BOOST_REQUIRE(!l.try_hold_write_lock());

    opt_rl->return_all();
    auto opt_wl = l.try_hold_write_lock();
    BOOST_REQUIRE(opt_wl.has_value());

    BOOST_REQUIRE(!l.try_hold_read_lock());
    BOOST_REQUIRE(!l.try_hold_write_lock());
}

SEASTAR_THREAD_TEST_CASE(test_failed_with_lock) {
    struct test_lock {
        future<> lock() noexcept {
            return make_exception_future<>(std::runtime_error("injected"));
        }
        void unlock() noexcept {
            BOOST_REQUIRE(false);
        }
    };

    test_lock l;

    // if l.lock() fails neither the function nor l.unlock()
    // should be called.
    BOOST_REQUIRE_THROW(with_lock(l, [] {
        BOOST_REQUIRE(false);
    }).get(), std::runtime_error);
}

SEASTAR_THREAD_TEST_CASE(test_shared_mutex) {
    shared_mutex sm;

    sm.lock().get();
    BOOST_REQUIRE(!sm.try_lock());
    BOOST_REQUIRE(!sm.try_lock_shared());
    sm.unlock();

    sm.lock_shared().get();
    BOOST_REQUIRE(!sm.try_lock());
    BOOST_REQUIRE(sm.try_lock_shared());
    sm.lock_shared().get();
    sm.unlock_shared();
    sm.unlock_shared();
    sm.unlock_shared();

    BOOST_REQUIRE(sm.try_lock());
    sm.unlock();
}

SEASTAR_TEST_CASE(test_shared_mutex_exclusive) {
    return do_with(shared_mutex(), unsigned(0), [] (shared_mutex& sm, unsigned& counter) {
        return parallel_for_each(std::views::iota(0, 10), [&sm, &counter] (int idx) {
            return with_lock(sm, [&counter] {
                BOOST_REQUIRE_EQUAL(counter, 0u);
                ++counter;
                return sleep(1ms).then([&counter] {
                    --counter;
                    BOOST_REQUIRE_EQUAL(counter, 0u);
                });
            });
        });
    });
}

SEASTAR_TEST_CASE(test_shared_mutex_shared) {
    return do_with(shared_mutex(), unsigned(0), unsigned(0), [] (shared_mutex& sm, unsigned& counter, unsigned& max) {
        return parallel_for_each(std::views::iota(0, 10), [&sm, &counter, &max] (int idx) {
            return with_shared(sm, [&counter, &max] {
                ++counter;
                max = std::max(max, counter);
                return sleep(1ms).then([&counter] {
                    --counter;
                });
            });
        }).finally([&counter, &max] {
            BOOST_REQUIRE_EQUAL(counter, 0u);
            BOOST_REQUIRE_NE(max, 0u);
        });
    });
}

SEASTAR_THREAD_TEST_CASE(test_shared_mutex_failed_func) {
    shared_mutex sm;

    // verify that the shared_mutex is unlocked when func fails
    future<> fut = with_shared(sm, [] {
        throw std::runtime_error("injected");
    });
    BOOST_REQUIRE_THROW(fut.get(), std::runtime_error);

    fut = with_lock(sm, [] {
        throw std::runtime_error("injected");
    });
    BOOST_REQUIRE_THROW(fut.get(), std::runtime_error);

    BOOST_REQUIRE(sm.try_lock());
    sm.unlock();
}

SEASTAR_THREAD_TEST_CASE(test_shared_mutex_throwing_func) {
    shared_mutex sm;
    struct X {
        int x;
        X(int x_) noexcept : x(x_) {};
        X(X&& o) : x(o.x) {
            throw std::runtime_error("X moved");
        }
    };

    // verify that the shared_mutex is unlocked when func move fails
    future<> fut = with_shared(sm, [x = X(0)] {});
    BOOST_REQUIRE_THROW(fut.get(), std::runtime_error);

    fut = with_lock(sm, [x = X(0)] {});
    BOOST_REQUIRE_THROW(fut.get(), std::runtime_error);

    BOOST_REQUIRE(sm.try_lock());
    sm.unlock();
}

SEASTAR_THREAD_TEST_CASE(test_shared_mutex_failed_lock) {
#ifdef SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION
    shared_mutex sm;

    // if l.lock() fails neither the function nor l.unlock()
    // should be called.
    sm.lock().get();
    seastar::memory::local_failure_injector().fail_after(0);
    BOOST_REQUIRE_THROW(with_shared(sm, [] {
        BOOST_REQUIRE(false);
    }).get(), std::bad_alloc);

    seastar::memory::local_failure_injector().fail_after(0);
    BOOST_REQUIRE_THROW(with_lock(sm, [] {
        BOOST_REQUIRE(false);
    }).get(), std::bad_alloc);
    sm.unlock();

    seastar::memory::local_failure_injector().cancel();
#endif // SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION
}

struct expected_exception : public std::exception {
    int value;
    expected_exception(int v) noexcept : value(v) {}
};

struct moved_exception : public std::exception {
    int count;
    moved_exception(int c) noexcept : count(c) {}
};

struct throw_on_move {
    int value;
    int delay;
    int count = 0;

    throw_on_move(int v, int d = 0) noexcept : value(v), delay(d) {}
    throw_on_move(const throw_on_move& o) = default;
    throw_on_move(throw_on_move&& o)
        : value(o.value)
        , delay(o.delay)
        , count(o.count + 1)
    {
        if (count >= delay) {
            throw moved_exception(count);
        }
    }
};

SEASTAR_THREAD_TEST_CASE(test_with_shared_typed_return_nothrow_move_func) {
    shared_mutex sm;

    auto expected = 42;
    auto res = with_shared(sm, [expected] {
        return expected;
    }).get();
    BOOST_REQUIRE_EQUAL(res, expected);

    try {
        with_shared(sm, [expected] {
            if (expected == 42) {
                throw expected_exception(expected);
            }
            return expected;
        }).get();
        BOOST_FAIL("No exception was thrown");
    } catch (const expected_exception& e) {
        BOOST_REQUIRE_EQUAL(e.value, expected);
    } catch (const std::exception& e) {
        BOOST_FAIL(format("Unexpected exception type: {}", e.what()));
    }
}

SEASTAR_THREAD_TEST_CASE(test_with_shared_typed_return_throwing_move_func) {
    shared_mutex sm;

    int expected_value = 42;
    bool done = false;
    for (int move_delay = 0; !done; move_delay++) {
        try {
            auto res = with_shared(sm, [exp = throw_on_move(expected_value, move_delay)] {
                auto expected = std::move(exp);
                return expected.value;
            }).get();
            BOOST_REQUIRE_EQUAL(res, expected_value);
            done = true;
        } catch (const moved_exception& e) {
        } catch (const std::exception& e) {
            BOOST_FAIL(format("Unexpected exception type: {}", e.what()));
        }
    }
}

SEASTAR_THREAD_TEST_CASE(test_with_lock_typed_return_nothrow_move_func) {
    shared_mutex sm;

    auto expected = 42;
    auto res = with_lock(sm, [expected] {
        return expected;
    }).get();
    BOOST_REQUIRE_EQUAL(res, expected);

    try {
        with_lock(sm, [expected] {
            if (expected == 42) {
                throw expected_exception(expected);
            }
            return expected;
        }).get();
        BOOST_FAIL("No exception was thrown");
    } catch (const expected_exception& e) {
        BOOST_REQUIRE_EQUAL(e.value, expected);
    } catch (const std::exception& e) {
        BOOST_FAIL(format("Unexpected exception type: {}", e.what()));
    }
}

SEASTAR_THREAD_TEST_CASE(test_with_lock_typed_return_throwing_move_func) {
    shared_mutex sm;

    int expected_value = 42;
    bool done = false;
    for (int move_delay = 0; !done; move_delay++) {
        try {
            auto res = with_lock(sm, [exp = throw_on_move(expected_value, move_delay)] {
                auto expected = std::move(exp);
                return expected.value;
            }).get();
            BOOST_REQUIRE_EQUAL(res, expected_value);
            done = true;
        } catch (const moved_exception& e) {
        } catch (const std::exception& e) {
            BOOST_FAIL(format("Unexpected exception type: {}", e.what()));
        }
    }
}

SEASTAR_TEST_CASE(test_shared_mutex_locks) {
    shared_mutex sm;

    {
        const auto ulock = co_await get_unique_lock(sm);
        BOOST_REQUIRE(!sm.try_lock());
        BOOST_REQUIRE(!sm.try_lock_shared());
    }

    BOOST_REQUIRE(sm.try_lock());
    sm.unlock();

    {
        const auto slock1 = co_await get_shared_lock(sm);
        BOOST_REQUIRE(!sm.try_lock());
        BOOST_REQUIRE(sm.try_lock_shared());

        const auto slock2 = co_await get_shared_lock(sm);

        // This balances out the call to `try_lock_shared()` above.
        sm.unlock_shared();
    }

    BOOST_REQUIRE(sm.try_lock());
    sm.unlock();
}

SEASTAR_TEST_CASE(test_shared_mutex_exclusive_locks) {
    shared_mutex sm{};
    unsigned counter = 0;

    co_await coroutine::parallel_for_each(std::views::iota(0, 10), coroutine::lambda([&sm, &counter] (auto&&) -> future<> {
        const auto ulock = co_await get_unique_lock(sm);
        BOOST_REQUIRE_EQUAL(counter, 0u);
        ++counter;
        co_await sleep(1ms);
        --counter;
        BOOST_REQUIRE_EQUAL(counter, 0u);
    }));
}

SEASTAR_TEST_CASE(test_shared_mutex_exception_locks) {
    shared_mutex sm;

    // Verify that the shared_mutex is unlocked when an exception is thrown.
    try {
        const auto slock = co_await get_shared_lock(sm);
        throw std::runtime_error("injected");
    } catch (const std::runtime_error&) {
        BOOST_REQUIRE(sm.try_lock());
        sm.unlock();
    } catch (...) {
        BOOST_FAIL(format("Unexpected exception type: {}", std::current_exception()));
    }

    try {
        const auto ulock = co_await get_unique_lock(sm);
        throw std::runtime_error("injected");
    } catch (const std::runtime_error&) {
        BOOST_REQUIRE(sm.try_lock());
        sm.unlock();
    } catch (...) {
        BOOST_FAIL(format("Unexpected exception type: {}", std::current_exception()));
    }

    BOOST_REQUIRE(sm.try_lock());
    sm.unlock();
}
