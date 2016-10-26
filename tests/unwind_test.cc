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
 * Copyright 2016 ScyllaDB
 */

#define BOOST_TEST_MODULE core

#include <boost/test/included/unit_test.hpp>
#include <pthread.h>
#include "util/defer.hh"
#include "core/posix.hh"
#include "util/backtrace.hh"

void foo() {
    throw std::runtime_error("foo");
}

// Exploits issue #1725
BOOST_AUTO_TEST_CASE(test_signal_mask_is_preserved_on_unwinding) {
    sigset_t mask;
    sigset_t old;
    sigfillset(&mask);
    auto res = ::pthread_sigmask(SIG_SETMASK, &mask, &old);
    throw_pthread_error(res);

    // Check which signals we actually managed to block
    res = ::pthread_sigmask(SIG_SETMASK, NULL, &mask);
    throw_pthread_error(res);

    try {
        foo();
    } catch (...) {
        // ignore
    }

    // Check backtrace()
    {
        size_t count = 0;
        backtrace([&count] (auto) { ++count; });
        BOOST_REQUIRE(count > 0);
    }

    {
        sigset_t mask2;
        auto res = ::pthread_sigmask(SIG_SETMASK, &old, &mask2);
        throw_pthread_error(res);

        for (int i = 1; i < NSIG; ++i) {
            BOOST_REQUIRE(sigismember(&mask2, i) == sigismember(&mask, i));
        }
    }
}
