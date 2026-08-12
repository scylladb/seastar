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

#include <limits.h>
#include <sys/uio.h>

#include <algorithm>
#include <vector>

#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/seastar.hh>
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

#if SEASTAR_API_LEVEL >= 9

// One 1-byte iovec per element, each holding a distinct byte value, so the
// bytes that arrive at the other end identify which iovecs were submitted.
// The total stays under PIPE_BUF, which makes a pipe write of the whole set
// atomic: with an empty pipe it either writes everything or nothing, so the
// byte counts below are exact rather than best-effort.
static std::vector<iovec> make_iovecs(std::vector<char>& data) {
    std::vector<iovec> iovs;
    iovs.reserve(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = char('a' + i % 26);
        iovs.push_back({&data[i], 1});
    }
    return iovs;
}

// The kernel rejects a vectored I/O with more than IOV_MAX iovecs: both
// writev(2) and IORING_OP_WRITEV fail such a call with EINVAL. Backends
// therefore submit only the first IOV_MAX iovecs and report a short write,
// which is what every caller already handles.
//
// The bug this guards against was specific to the asymmetric_io_uring backend,
// which passed the whole span to the kernel and failed the write outright, but
// the semantic is common to all backends so the test is backend-agnostic. To
// exercise the backend that regressed, run it with
// `--reactor-backend=asymmetric_io_uring --async-workers-cpuset=<cpus>`.
SEASTAR_TEST_CASE(writev_over_iov_max_is_short_write) {
    auto [read_fd, write_fd] = co_await make_pipe();
    pollable_fd writer(std::move(write_fd));

    std::vector<char> data(IOV_MAX + 1);
    auto iovs = make_iovecs(data);

    const size_t written = co_await writer.write_some(iovs);
    BOOST_REQUIRE_EQUAL(written, IOV_MAX);

    std::vector<char> buf(data.size());
    BOOST_REQUIRE_EQUAL(::read(read_fd.get(), buf.data(), buf.size()), ssize_t(IOV_MAX));
    BOOST_REQUIRE(std::equal(buf.begin(), buf.begin() + IOV_MAX, data.begin()));
}

// The short write above loses no data: write_all() trims the completed prefix
// and resubmits the rest, so an oversized span still arrives in full.
SEASTAR_TEST_CASE(write_all_over_iov_max_writes_everything) {
    auto [read_fd, write_fd] = co_await make_pipe();
    pollable_fd writer(std::move(write_fd));

    std::vector<char> data(2 * IOV_MAX);
    auto iovs = make_iovecs(data);

    co_await writer.write_all(iovs);

    std::vector<char> buf(data.size());
    BOOST_REQUIRE_EQUAL(::read(read_fd.get(), buf.data(), buf.size()), ssize_t(data.size()));
    BOOST_REQUIRE(buf == data);
}

#endif
