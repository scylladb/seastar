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
 * Copyright (C) 2021 ScyllaDB
 */

#include <seastar/core/thread.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/file.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/internal/io_request.hh>
#include <seastar/core/internal/io_sink.hh>

using namespace seastar;

template <size_t Len>
struct fake_file {
    int data[Len] = {};

    static internal::io_request make_write_req(size_t idx, int val) {
        int* buf = new int(val);
        return internal::io_request::make_write(0, idx, buf, 1);
    }

    void execute_write_req(internal::io_request& rq, io_completion* desc) {
        data[rq.pos()] = *(reinterpret_cast<int*>(rq.address()));
        desc->complete_with(rq.size());
    }
};

struct io_queue_for_tests {
    io_group_ptr group;
    internal::io_sink sink;
    io_queue queue;

    io_queue_for_tests()
        : group(std::make_shared<io_group>(io_group::config{}))
        , sink()
        , queue(group, sink, io_queue::config{0})
    {}
};

SEASTAR_THREAD_TEST_CASE(test_basic_flow) {
    io_queue_for_tests tio;
    fake_file<1> file;

    auto f = tio.queue.queue_request(default_priority_class(), 0, file.make_write_req(0, 42))
    .then([&file] (size_t len) {
        BOOST_REQUIRE(file.data[0] == 42);
    });

    tio.queue.poll_io_queue();
    tio.sink.drain([&file] (internal::io_request& rq, io_completion* desc) -> bool {
        file.execute_write_req(rq, desc);
        return true;
    });

    f.get();
}
