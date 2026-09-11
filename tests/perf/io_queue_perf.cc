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

#include <seastar/testing/perf_tests.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/io_intent.hh>
#include <seastar/core/internal/io_request.hh>
#include <seastar/core/internal/io_sink.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/shared_ptr.hh>

using namespace seastar;

// Minimal harness copied from tests/unit/io_queue_test.cc; the struct name
// is friended by io_queue/io_group, letting kick() drive the throttler
// directly instead of at simulated real-time disk bandwidth.
struct io_queue_for_tests {
    io_group_ptr group;
    internal::io_sink sink;
    io_queue queue;
    timer<> kicker;

    io_queue_for_tests(const io_queue::config& cfg = io_queue::config{0})
        : group(std::make_shared<io_group>(cfg, 1))
        , sink()
        , queue(group, sink)
        , kicker([this] { kick(); })
    {
        kicker.arm_periodic(std::chrono::microseconds(500));
    }

    void kick() {
        for (auto&& fg : group->_fgs) {
            fg.replenish_capacity(std::chrono::steady_clock::now());
        }
    }

    future<size_t> queue_request(internal::priority_class pc, internal::io_direction_and_length dnl, internal::io_request req, io_intent* intent, iovec_keeper iovs) noexcept {
        return queue.queue_request(pc, dnl, std::move(req), intent, std::move(iovs));
    }
};

// Drives many small writes through one io_queue with a fake, in-line-
// completing sink (no real file/disk), so only io_queue's own per-request
// bookkeeping is timed. Queued in batches of `batch_size` for real fair-queue
// contention rather than a trivial one-at-a-time round trip.
struct perf_io_queue : io_queue_for_tests {
    static constexpr unsigned batch_size = 512;

    std::vector<int> buf;

    perf_io_queue()
        : io_queue_for_tests()
        , buf(batch_size, 42)
    { }

    future<size_t> submit_batch() {
        auto pc = internal::priority_class(current_scheduling_group());
        auto dnl = internal::io_direction_and_length(internal::io_direction_and_length::write_idx, sizeof(int));

        std::vector<future<size_t>> reqs;
        reqs.reserve(batch_size);
        for (unsigned i = 0; i < batch_size; i++) {
            auto req = internal::io_request::make_write(0, i, &buf[i], sizeof(int), false);
            reqs.push_back(queue_request(pc, dnl, std::move(req), nullptr, {}));
        }

        // Concurrently: drain (poll + complete) until every submitted
        // request has been accounted for.
        auto completed = make_lw_shared<unsigned>(0);
        auto drained = do_until([completed] { return *completed >= batch_size; }, [this, completed] {
            queue.poll_io_queue();
            *completed += sink.drain([] (const internal::io_request& rq, io_completion* desc) -> bool {
                const auto& op = rq.as<internal::io_request::operation::write>();
                desc->complete_with(op.size);
                return true;
            });
            return make_ready_future<>();
        });

        auto submitted = when_all_succeed(reqs.begin(), reqs.end()).discard_result();

        return when_all_succeed(std::move(submitted), std::move(drained)).discard_result().then([] {
            return size_t(batch_size);
        });
    }
};

PERF_TEST_F(perf_io_queue, submit_dispatch_complete)
{
    return submit_batch();
}
