/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License"). See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership. You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 * Copyright 2022-present ScyllaDB
 */

#include <boost/test/tools/old/interface.hpp>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

#include "seastar/core/future.hh"
#include <seastar/core/thread.hh>
#include <seastar/core/internal/pollable_fd.hh>

using namespace seastar;

SEASTAR_TEST_CASE(ongoing_readable_close_aborted_test) {
    return async([]() {
        int pipe_ends[2] = {};

        BOOST_REQUIRE_EQUAL(pipe2(pipe_ends, O_NONBLOCK|O_DIRECT|O_CLOEXEC), 0);

        auto write_end = pollable_fd(file_desc::from_fd(pipe_ends[1]));
        auto read_end = pollable_fd(file_desc::from_fd(pipe_ends[0]));

        char buf[32];
        auto future_read = read_end.read_some(buf, sizeof(buf));
        read_end.close();

        BOOST_REQUIRE_THROW(future_read.get() , broken_promise);
    });
}
