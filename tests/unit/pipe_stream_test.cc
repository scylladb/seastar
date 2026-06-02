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
 * Copyright (C) 2026 ScyllaDB
 */

#include <sys/ioctl.h>
#include <termios.h>
#include "test_comparisons.hh"

#include <seastar/core/fstream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sstring.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

using namespace seastar;
using seastar::experimental::make_pipe;

// ---------------------------------------------------------------------------
// pipe_stream_roundtrip
//
// Exercises pipe_data_source_impl / pipe_data_sink_impl using an
// anonymous OS pipe obtained from reactor::make_pipe().  The write end is
// wrapped in make_pipe_output_stream() and the read end in
// make_pipe_input_stream().
// ---------------------------------------------------------------------------
SEASTAR_THREAD_TEST_CASE(pipe_stream_roundtrip) {
    auto [read_fd, write_fd] = make_pipe().get();

    auto in  = make_pipe_input_stream (std::move(read_fd));
    auto out = make_pipe_output_stream(std::move(write_fd));

    const sstring payload = "Hello, pipe streams!";

    out.write(payload).get();
    // flush() pushes buffered data into the OS pipe buffer; after this the
    // read end can see it without the write end being closed.
    out.flush().get();

    auto buf = in.read_exactly(payload.size()).get();
    SEASTAR_BOOST_REQUIRE_EQUAL(sstring(buf.get(), buf.size()), payload);

    out.close().get();
    in.close().get();
}

// ---------------------------------------------------------------------------
// pipe_pty_stream_roundtrip
//
// Opens a pseudo-terminal pair via POSIX APIs, wraps each end in a chardev
// stream, and verifies a data round-trip through the PTY line discipline.
//
// PTY setup is done with raw POSIX calls (posix_openpt / grantpt / unlockpt /
// ptsname / TCGETS+cfmakeraw+TCSETS) so that no Seastar file abstraction is
// required for the setup step.  The resulting fds are then owned by file_desc
// objects and passed to make_chardev_input/output_stream.
// ---------------------------------------------------------------------------
static std::pair<file_desc, file_desc> open_pty_pair() {
    // Open master PTY.
    int master_raw = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    throw_system_error_on(master_raw == -1, "posix_openpt");
    auto master = file_desc::from_fd(master_raw);

    // grantpt is a no-op on modern Linux with udev; unlockpt enables the slave.
    throw_system_error_on(::grantpt (master.get()) == -1, "grantpt");
    throw_system_error_on(::unlockpt(master.get()) == -1, "unlockpt");

    // Determine the slave device path.
    char slave_buf[64];
    throw_system_error_on(::ptsname_r(master.get(), slave_buf, sizeof(slave_buf)) != 0,
                          "ptsname_r");

    // Open slave PTY.
    int slave_raw = ::open(slave_buf, O_RDWR | O_NOCTTY | O_CLOEXEC);
    throw_system_error_on(slave_raw == -1, "open slave pty");
    auto slave = file_desc::from_fd(slave_raw);

    // Put the slave in raw mode so the line discipline passes data unmodified.
    struct termios t;
    throw_system_error_on(::ioctl(slave.get(), TCGETS, &t) == -1, "TCGETS");
    ::cfmakeraw(&t);
    throw_system_error_on(::ioctl(slave.get(), TCSETS, &t) == -1, "TCSETS");

    return {std::move(master), std::move(slave)};
}

SEASTAR_THREAD_TEST_CASE(pipe_pty_stream_roundtrip) {
    auto [master, slave] = open_pty_pair();

    // slave writes → data flows through PTY → master reads.
    auto out = make_pipe_output_stream(std::move(slave));
    auto in  = make_pipe_input_stream (std::move(master));

    const sstring payload = "Hello, PTY streams!";

    out.write(payload).get();
    out.flush().get();

    auto buf = in.read_exactly(payload.size()).get();
    SEASTAR_BOOST_REQUIRE_EQUAL(sstring(buf.get(), buf.size()), payload);

    out.close().get();
    in.close().get();
}

