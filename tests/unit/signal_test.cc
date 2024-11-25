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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include <seastar/core/signal.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/do_with.hh>
#include <seastar/testing/test_case.hh>

using namespace seastar;

extern "C" {
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
}

SEASTAR_TEST_CASE(test_sighup) {
    return do_with(make_lw_shared<promise<>>(), false, [](auto const& p, bool& signaled) {
        seastar::handle_signal(SIGHUP, [p, &signaled] {
            signaled = true;
            p->set_value();
        });

        kill(getpid(), SIGHUP);

        return p->get_future().then([&] {
            BOOST_REQUIRE_EQUAL(signaled, true);
        });
    });
}
