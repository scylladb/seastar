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
#include <seastar/core/sleep.hh>
#include <seastar/util/assert.hh>
#include <sys/mman.h>
#include <signal.h>

#include <valgrind/valgrind.h>

using namespace seastar;
using namespace std::chrono_literals;

SEASTAR_TEST_CASE(test_thread_1) {
    return do_with(sstring(), [] (sstring& x) {
        auto t1 = new thread([&x] {
            x = "abc";
        });
        return t1->join().then([&x, t1] {
            BOOST_REQUIRE_EQUAL(x, "abc");
            delete t1;
        });
    });
}

SEASTAR_TEST_CASE(test_thread_2) {
    struct tmp {
        std::vector<thread> threads;
        semaphore sem1{0};
        semaphore sem2{0};
        int counter = 0;
        void thread_fn() {
            sem1.wait(1).get();
            ++counter;
            sem2.signal(1);
        }
    };
    return do_with(tmp(), [] (tmp& x) {
        auto n = 10;
        for (int i = 0; i < n; ++i) {
            x.threads.emplace_back(std::bind(&tmp::thread_fn, &x));
        }
        BOOST_REQUIRE_EQUAL(x.counter, 0);
        x.sem1.signal(n);
        return x.sem2.wait(n).then([&x, n] {
            BOOST_REQUIRE_EQUAL(x.counter, n);
            return parallel_for_each(x.threads.begin(), x.threads.end(), std::mem_fn(&thread::join));
        });
    });
}

SEASTAR_TEST_CASE(test_thread_async) {
    sstring x = "x";
    sstring y = "y";
    auto concat = [] (sstring x, sstring y) {
        sleep(10ms).get();
        return x + y;
    };
    return async(concat, x, y).then([] (sstring xy) {
        BOOST_REQUIRE_EQUAL(xy, "xy");
    });
}

SEASTAR_TEST_CASE(test_thread_async_immed) {
    return async([] { return 3; }).then([] (int three) {
        BOOST_REQUIRE_EQUAL(three, 3);
    });
}

SEASTAR_TEST_CASE(test_thread_async_nested) {
    return async([] {
        return async([] {
            return 3;
        }).get();
    }).then([] (int three) {
        BOOST_REQUIRE_EQUAL(three, 3);
    });
}

void compute(float& result, bool& done, uint64_t& ctr) {
    while (!done) {
        for (int n = 0; n < 10000; ++n) {
            result += 1 / (result + 1);
            ++ctr;
        }
        thread::yield();
    }
}

#if defined(SEASTAR_ASAN_ENABLED) && defined(SEASTAR_HAVE_ASAN_FIBER_SUPPORT)
volatile int force_write;
volatile void* shut_up_gcc;

[[gnu::noinline]]
void throw_exception() {
    volatile char buf[1024];
    shut_up_gcc = &buf;
    for (int i = 0; i < 1024; i++) {
        buf[i] = force_write;
    }
    throw 1;
}

[[gnu::noinline]]
void use_stack() {
    volatile char buf[2 * 1024];
    shut_up_gcc = &buf;
    for (int i = 0; i < 2 * 1024; i++) {
        buf[i] = force_write;
    }
}

SEASTAR_TEST_CASE(test_asan_false_positive) {
    return async([] {
        try {
            throw_exception();
        } catch (...) {
            use_stack();
        }
    });
}
#endif

SEASTAR_THREAD_TEST_CASE(abc, *boost::unit_test::expected_failures(2)) {
    BOOST_TEST(false);
    BOOST_TEST(false);
}

SEASTAR_TEST_CASE(test_thread_custom_stack_size) {
    sstring x = "x";
    sstring y = "y";
    auto concat = [] (sstring x, sstring y) {
        sleep(10ms).get();
        return x + y;
    };
    thread_attributes attr;
    attr.stack_size = 16384;
    return async(attr, concat, x, y).then([] (sstring xy) {
        BOOST_REQUIRE_EQUAL(xy, "xy");
    });
}

// The test case uses x86_64 specific signal handler info. The test
// fails with detect_stack_use_after_return=1. We could put it behind
// a command line option and fork/exec to run it after removing
// detect_stack_use_after_return=1 from the environment.
#if defined(SEASTAR_THREAD_STACK_GUARDS) && defined(__x86_64__) && !defined(SEASTAR_ASAN_ENABLED)
struct test_thread_custom_stack_size_failure : public seastar::testing::seastar_test {
    using seastar::testing::seastar_test::seastar_test;
    seastar::future<> run_test_case() const override;
};

static test_thread_custom_stack_size_failure test_thread_custom_stack_size_failure_instance(
    "test_thread_custom_stack_size_failure",
    __FILE__, __LINE__);
static thread_local volatile bool stack_guard_bypassed = false;

static int get_mprotect_flags(void* ctx) {
    int flags;
    ucontext_t* context = reinterpret_cast<ucontext_t*>(ctx);
    if (context->uc_mcontext.gregs[REG_ERR] & 0x2) {
        flags = PROT_READ | PROT_WRITE;
    } else {
        flags = PROT_READ;
    }
    return flags;
}

static void* pagealign(void* ptr, size_t page_size) {
    static const int pageshift = ffs(page_size) - 1;
    return reinterpret_cast<void*>(((reinterpret_cast<intptr_t>((ptr)) >> pageshift) << pageshift));
}

static thread_local struct sigaction default_old_sigsegv_handler;

static void bypass_stack_guard(int sig, siginfo_t* si, void* ctx) {
    SEASTAR_ASSERT(sig == SIGSEGV);
    int flags = get_mprotect_flags(ctx);
    stack_guard_bypassed = (flags & PROT_WRITE);
    if (!stack_guard_bypassed) {
        return;
    }
    size_t page_size = getpagesize();
    auto mp_result = mprotect(pagealign(si->si_addr, page_size), page_size, PROT_READ | PROT_WRITE);
    SEASTAR_ASSERT(mp_result == 0);
}

// This test will fail with a regular stack size, because we only probe
// around 10KiB of data, and the stack guard resides after 128'th KiB.
seastar::future<> test_thread_custom_stack_size_failure::run_test_case() const {
    if (RUNNING_ON_VALGRIND) {
        return make_ready_future<>();
    }

    sstring x = "x";
    sstring y = "y";

    // Catch segmentation fault once:
    struct sigaction sa{};
    sa.sa_sigaction = &bypass_stack_guard;
    sa.sa_flags = SA_SIGINFO;
    auto ret = sigaction(SIGSEGV, &sa, &default_old_sigsegv_handler);
    if (ret) {
        throw std::system_error(ret, std::system_category());
    }

    auto concat = [] (sstring x, sstring y) {
        sleep(10ms).get();
        // Probe the stack by writing to it in intervals of 1024,
        // until we hit a write fault. In order not to ruin anything,
        // the "write" uses data it just read from the address.
        volatile char* mem = reinterpret_cast<volatile char*>(&x);
        for (int i = 0; i < 20; ++i) {
            mem[i*-1024] = char(mem[i*-1024]);
            if (stack_guard_bypassed) {
                break;
            }
        }
        return x + y;
    };
    thread_attributes attr;
    attr.stack_size = 16384;
    return async(attr, concat, x, y).then([] (sstring xy) {
        BOOST_REQUIRE_EQUAL(xy, "xy");
        BOOST_REQUIRE(stack_guard_bypassed);
        auto ret = sigaction(SIGSEGV, &default_old_sigsegv_handler, nullptr);
        if (ret) {
            throw std::system_error(ret, std::system_category());
        }
    }).then([concat, x, y] {
        // The same function with a default stack will not trigger
        // a segfault, because its stack is much bigger than 10KiB
        return async(concat, x, y).then([] (sstring xy) {
            BOOST_REQUIRE_EQUAL(xy, "xy");
        });
    });
}
#endif // SEASTAR_THREAD_STACK_GUARDS && __x86_64__
