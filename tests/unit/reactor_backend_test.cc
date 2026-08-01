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
 * Copyright (C) 2026 Kefu Chai (tchaikov@gmail.com)
 */

#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/internal/pollable_fd.hh>
#include <seastar/testing/test_case.hh>

using namespace seastar;

// After a co_await on a pollable_fd future resolves, await_resume() calls
// future::get() which calls get_available_state_ref(), which sees
// _future._promise != nullptr and calls detach_promise() — setting the
// embedded promise's _state to nullptr.  A subsequent poll() on the same fd
// reuses the same pollable_fd_state_completion object; without the fix it
// calls get_future() on the already-consumed promise and hits
// SEASTAR_ASSERT(this->_state).
//
// The test uses co_await pfd.readable() directly (not the higher-level
// read_some path, which uses .then() internally and leaves _state non-null)
// so that the co_await machinery triggers the detach_promise() path.
//
// The bug only manifests under the io_uring backend, but the test is
// backend-agnostic so it is exercised for all available backends.
SEASTAR_TEST_CASE(pollable_fd_state_completion_reuse_test) {
    int sv[2];
    BOOST_REQUIRE_EQUAL(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv), 0);

    pollable_fd reader(file_desc::from_fd(sv[0]));

    // Write two bytes so both polls below complete without blocking.
    const char data[] = "ab";
    BOOST_REQUIRE_EQUAL(::write(sv[1], data, 2), 2);

    // First co_await readable() — first use of _completion_pollin.
    // await_resume() calls detach_promise(), setting _pr._state = nullptr.
    co_await reader.readable();

    // Second co_await readable() — reuses the same _completion_pollin.
    // Without the fix, get_future() hits SEASTAR_ASSERT(this->_state) under
    // the io_uring backend.
    co_await reader.readable();

    ::close(sv[1]);
    co_return;
}
