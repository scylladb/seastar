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
#include <seastar/core/when_all.hh>
#include <seastar/core/file.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/io_intent.hh>
#include <seastar/core/internal/io_request.hh>
#include <seastar/core/internal/io_sink.hh>

using namespace seastar;

template <size_t Len>
struct fake_file {
    int data[Len] = {};

    static internal::io_request make_write_req(size_t idx, int* buf) {
        return internal::io_request::make_write(0, idx, buf, 1, false);
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

    auto val = std::make_unique<int>(42);
    auto f = tio.queue.queue_request(default_priority_class(), 0, file.make_write_req(0, val.get()), nullptr)
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

SEASTAR_THREAD_TEST_CASE(test_intent_safe_ref) {
    auto get_cancelled = [] (internal::intent_reference& iref) -> bool {
        try {
            iref.retrieve();
            return false;
        } catch(seastar::cancelled_error& err) {
            return true;
        }
    };

    io_intent intent, intent_x;

    internal::intent_reference ref_orig(&intent);
    BOOST_REQUIRE(ref_orig.retrieve() == &intent);

    // Test move armed
    internal::intent_reference ref_armed(std::move(ref_orig));
    BOOST_REQUIRE(ref_orig.retrieve() == nullptr);
    BOOST_REQUIRE(ref_armed.retrieve() == &intent);

    internal::intent_reference ref_armed_2(&intent_x);
    ref_armed_2 = std::move(ref_armed);
    BOOST_REQUIRE(ref_armed.retrieve() == nullptr);
    BOOST_REQUIRE(ref_armed_2.retrieve() == &intent);

    intent.cancel();
    BOOST_REQUIRE(get_cancelled(ref_armed_2));

    // Test move cancelled
    internal::intent_reference ref_cancelled(std::move(ref_armed_2));
    BOOST_REQUIRE(ref_armed_2.retrieve() == nullptr);
    BOOST_REQUIRE(get_cancelled(ref_cancelled));

    internal::intent_reference ref_cancelled_2(&intent_x);
    ref_cancelled_2 = std::move(ref_cancelled);
    BOOST_REQUIRE(ref_cancelled.retrieve() == nullptr);
    BOOST_REQUIRE(get_cancelled(ref_cancelled_2));

    // Test move empty
    internal::intent_reference ref_empty(std::move(ref_orig));
    BOOST_REQUIRE(ref_empty.retrieve() == nullptr);

    internal::intent_reference ref_empty_2(&intent_x);
    ref_empty_2 = std::move(ref_empty);
    BOOST_REQUIRE(ref_empty_2.retrieve() == nullptr);
}

static constexpr int nr_requests = 24;

SEASTAR_THREAD_TEST_CASE(test_io_cancellation) {
    fake_file<nr_requests> file;

    io_queue_for_tests tio;
    io_priority_class pc0 = tio.queue.register_one_priority_class("a", 100);
    io_priority_class pc1 = tio.queue.register_one_priority_class("b", 100);

    size_t idx = 0;
    int val = 100;

    io_intent live, dead;

    std::vector<future<>> finished;
    std::vector<future<>> cancelled;

    auto queue_legacy_request = [&] (io_queue_for_tests& q, io_priority_class& pc) {
        auto buf = std::make_unique<int>(val);
        auto f = q.queue.queue_request(pc, 0, file.make_write_req(idx, buf.get()), nullptr)
            .then([&file, idx, val, buf = std::move(buf)] (size_t len) {
                BOOST_REQUIRE(file.data[idx] == val);
                return make_ready_future<>();
            });
        finished.push_back(std::move(f));
        idx++;
        val++;
    };

    auto queue_live_request = [&] (io_queue_for_tests& q, io_priority_class& pc) {
        auto buf = std::make_unique<int>(val);
        auto f = q.queue.queue_request(pc, 0, file.make_write_req(idx, buf.get()), &live)
            .then([&file, idx, val, buf = std::move(buf)] (size_t len) {
                BOOST_REQUIRE(file.data[idx] == val);
                return make_ready_future<>();
            });
        finished.push_back(std::move(f));
        idx++;
        val++;
    };

    auto queue_dead_request = [&] (io_queue_for_tests& q, io_priority_class& pc) {
        auto buf = std::make_unique<int>(val);
        auto f = q.queue.queue_request(pc, 0, file.make_write_req(idx, buf.get()), &dead)
            .then_wrapped([buf = std::move(buf)] (auto&& f) {
                try {
                    f.get();
                    BOOST_REQUIRE(false);
                } catch(...) {}
                return make_ready_future<>();
            })
            .then([&file, idx] () {
                BOOST_REQUIRE(file.data[idx] == 0);
            });
        cancelled.push_back(std::move(f));
        idx++;
        val++;
    };

    auto seed = std::random_device{}();
    std::default_random_engine reng(seed);
    std::uniform_int_distribution<> dice(0, 5);

    for (int i = 0; i < nr_requests; i++) {
        int pc = dice(reng) % 2;
        if (dice(reng) < 3) {
            fmt::print("queue live req to pc {}\n", pc);
            queue_live_request(tio, pc == 0 ? pc0 : pc1);
        } else if (dice(reng) < 5) {
            fmt::print("queue dead req to pc {}\n", pc);
            queue_dead_request(tio, pc == 0 ? pc0 : pc1);
        } else {
            fmt::print("queue legacy req to pc {}\n", pc);
            queue_legacy_request(tio, pc == 0 ? pc0 : pc1);
        }
    }

    dead.cancel();

    // cancelled requests must resolve right at once

    when_all_succeed(cancelled.begin(), cancelled.end()).get();

    tio.queue.poll_io_queue();
    tio.sink.drain([&file] (internal::io_request& rq, io_completion* desc) -> bool {
        file.execute_write_req(rq, desc);
        return true;
    });

    when_all_succeed(finished.begin(), finished.end()).get();
}
