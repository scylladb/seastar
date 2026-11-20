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

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <optional>
#include <ratio>
#include <thread>
#include <tuple>
#include <seastar/core/thread.hh>
#include <seastar/core/sleep.hh>
#include <seastar/testing/random.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/test_runner.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/file.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/io_intent.hh>
#include <seastar/core/disk_params.hh>
#include <seastar/core/smp_options.hh>
#include <seastar/core/reactor_config.hh>
#include <seastar/core/internal/io_request.hh>
#include <seastar/core/internal/io_sink.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/internal/iovec_utils.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/later.hh>
#include <seastar/util/integrated-length.hh>

using namespace seastar;

struct fake_file {
    std::unordered_map<uint64_t, int> data;

    static internal::io_request make_write_req(size_t idx, int* buf) {
        return internal::io_request::make_write(0, idx, buf, 1, false);
    }

    static internal::io_request make_writev_req(size_t idx, int* buf, size_t nr, size_t buf_len, std::vector<::iovec>& vecs) {
        vecs.reserve(nr);
        for (unsigned i = 0; i < nr; i++) {
            vecs.push_back({ &buf[i], buf_len });
        }
        return internal::io_request::make_writev(0, idx, vecs, false);
    }

    void execute_write_req(const internal::io_request& rq, io_completion* desc) {
        const auto& op = rq.as<internal::io_request::operation::write>();
        data[op.pos] = *(reinterpret_cast<int*>(op.addr));
        desc->complete_with(op.size);
    }

    void execute_writev_req(const internal::io_request& rq, io_completion* desc) {
        size_t len = 0;
        const auto& op = rq.as<internal::io_request::operation::writev>();
        for (unsigned i = 0; i < op.iov_len; i++) {
            data[op.pos + i] = *(reinterpret_cast<int*>(op.iovec[i].iov_base));
            len += op.iovec[i].iov_len;
        }
        desc->complete_with(len);
    }
};

struct io_queue_for_tests {
    io_group_ptr group;
    internal::io_sink sink;
    io_queue queue;
    timer<> kicker;

    // If `own_timers` is false, the tests of the rate correction call the tick
    // and the regulator of the queue themselves, with measured() and
    // regulate()
    io_queue_for_tests(const io_queue::config& cfg = io_queue::config{0}, const io_group* peer = nullptr, bool own_timers = false)
        : group(std::make_shared<io_group>(cfg, 1, peer))
        , sink()
        , queue(group, sink)
        , kicker([this] { kick(); })
    {
        kicker.arm_periodic(std::chrono::microseconds(500));
        if (cfg.disk_latency_threshold.count() > 0 && !own_timers) {
            queue._averaging_decay_timer.cancel();
            queue._regulator_timer.cancel();
        }
    }

    void kick() {
        for (auto&& fg : group->_fgs) {
            fg.replenish_capacity(std::chrono::steady_clock::now());
        }
    }

    future<size_t> queue_request(scheduling_group sg, internal::io_direction_and_length dnl, internal::io_request req, io_intent* intent, iovec_keeper iovs) noexcept {
        return queue.queue_request(sg, dnl, std::move(req), intent, std::move(iovs));
    }

    size_t max_request_length(int dnl_idx) const noexcept {
        return group->max_request_length(dnl_idx);
    }

    constexpr size_t request_length_limit() const noexcept {
        return io_group::request_length_limit;
    }

    void find_or_create_class(scheduling_group sg) {
        queue.find_or_create_class(sg);
    }

    fair_queue& get_fair_queue() {
        return queue._streams[0].fq;
    }

    double rate_correction() const {
        return queue._rate_correction;
    }

    uint64_t time_in_disk_us() const {
        return queue._time_in_disk_us;
    }

    uint64_t occupancy_us() const {
        return queue._occupancy_us;
    }

    uint64_t completed_in_disk() const {
        return queue._completed_in_disk;
    }

    fair_queue_entry::capacity_t sent() const {
        return queue._sent[0];
    }

    // Sets the measurements of the queue for the period that ends at `now`:
    // - `completed`: the number of requests that completed,
    // - `in_disk`: their summed time in the disk, in seconds,
    // - `sent`: the capacity that the queue sent, in seconds of the configured
    //   rate,
    // - `occupancy`: the time in the disk of all requests within the period,
    //   summed over the requests, in seconds. Without it, all of `in_disk` is
    //   in the period.
    // Then calls the tick of the queue at `now`, so that the queue writes its
    // report.
    void measured(uint64_t completed, double in_disk, double sent, io_queue::clock_type::time_point now, std::optional<double> occupancy = std::nullopt) {
        // Writes the report of the period before. At the first call, that is
        // the period in which the queue started. Then the values below are for
        // the period that ends at `now`.
        queue.report_rate_measurements(now - std::chrono::duration_cast<io_queue::clock_type::duration>(queue.get_config().rate_correction_period));
        queue._completed_in_disk += completed;
        queue._time_in_disk_us += in_disk * 1e6;
        queue._occupancy_us += occupancy.value_or(in_disk) * 1e6;
        queue._sent[0] += io_throttler::tokens_capacity(sent);
        queue.update_rate_correction(now);
    }

    // The time from the end of a period until the regulator reads its reports
    io_queue::clock_type::duration read_delay() const {
        return queue._regulator_delay;
    }

    // Runs the regulator at the time when its timer reads the reports of the
    // period that ends at `end`. Then calls the tick of the queue, so that the
    // queue reads the new c.
    void regulate(io_queue::clock_type::time_point end) {
        auto now = end + queue._regulator_delay;
        queue.run_regulator(now);
        queue._regulator_timer.cancel();
        queue.update_rate_correction(now);
    }

    // Runs the regulator at `now`, and returns the next time of its timer
    io_queue::clock_type::time_point next_regulation(io_queue::clock_type::time_point now) {
        queue.run_regulator(now);
        auto at = queue._regulator_timer.get_timeout();
        queue._regulator_timer.cancel();
        return io_queue::clock_type::time_point(at.time_since_epoch());
    }

    struct pending_waits {
        std::chrono::duration<double, std::milli> first_request;
        std::chrono::duration<double, std::milli> full_reservation;
    };

    pending_waits reserve_queued_capacity() {
        auto& stream = queue._streams[0];
        // Use up the initial burst allowance, as earlier I/O would.
        stream.out.replenish_capacity(io_queue::clock_type::now());
        stream.out.grab_capacity(stream.out.maximum_capacity());
        queue.poll_io_queue();
        auto* first = stream.fq.top();
        SEASTAR_ASSERT(first != nullptr);
        SEASTAR_ASSERT(stream._pending.cap == stream.fq.queued_capacity());
        return {stream.out.capacity_duration(first->capacity()),
                stream.out.capacity_duration(stream.out.capacity_deficiency(stream._pending.head))};
    }

    io_queue::clock_type::time_point next_pending_aio() {
        return queue.next_pending_aio();
    }

    bool has_pending_reservation() const {
        return queue._streams[0]._pending.cap != 0;
    }

    bool is_class_registered(scheduling_group sg) const noexcept {
        auto id = internal::scheduling_group_index(sg);
        return queue._priority_classes.size() > id && (queue._priority_classes[id] != nullptr);
    }
};

scheduling_group get_default_pc() {
    return current_scheduling_group();
}

SEASTAR_THREAD_TEST_CASE(test_basic_flow) {
    io_queue_for_tests tio;
    fake_file file;

    auto val = std::make_unique<int>(42);
    auto f = tio.queue_request(get_default_pc(), internal::io_direction_and_length(internal::io_direction_and_length::write_idx, 0), file.make_write_req(0, val.get()), nullptr, {})
    .then([&file] (size_t len) {
        BOOST_REQUIRE(file.data[0] == 42);
    });

    seastar::sleep(std::chrono::milliseconds(500)).get();
    tio.queue.poll_io_queue();
    tio.sink.drain([&file] (const internal::io_request& rq, io_completion* desc) -> bool {
        file.execute_write_req(rq, desc);
        return true;
    });

    f.get();
}

SEASTAR_THREAD_TEST_CASE(test_pending_io_wakes_for_first_request) {
    internal::disk_config_params disk_config(1);
    internal::disk_params disk;
    disk.read_bytes_rate = 1ul << 30;
    disk.write_bytes_rate = 1ul << 30;
    disk.read_req_rate = 1000;
    disk.write_req_rate = 1000;
    auto io_config = disk_config.generate_config(disk, 0, 1);
    io_config.mountpoint = "pending-first-request";
    io_config.rate_limit_duration = std::chrono::milliseconds(100);
    io_queue_for_tests tio(io_config);
    tio.kicker.cancel();

    constexpr size_t request_size = 4096;
    std::vector<future<size_t>> requests;
    for (unsigned i = 0; i < 8; ++i) {
        requests.push_back(tio.queue_request(get_default_pc(),
                internal::io_direction_and_length(internal::io_direction_and_length::write_idx, request_size),
                internal::io_request::make_write(0, i * request_size, nullptr, request_size, false),
                nullptr, {}));
    }

    auto waits = tio.reserve_queued_capacity();
    BOOST_REQUIRE(waits.full_reservation > waits.first_request * 4);
    auto wakeup = tio.next_pending_aio();
    auto after = io_queue::clock_type::now();
    BOOST_CHECK(wakeup <= after + waits.first_request * 2);

    for (unsigned i = 0; i < 100 && !std::all_of(requests.begin(), requests.end(),
            [] (const auto& request) { return request.available(); }); ++i) {
        tio.kick();
        tio.queue.poll_io_queue();
        tio.sink.drain([] (const internal::io_request& req, io_completion* desc) -> bool {
            desc->complete_with(req.as<internal::io_request::operation::write>().size);
            return true;
        });
        seastar::sleep(std::chrono::milliseconds(1)).get();
    }
    for (auto& request : requests) {
        BOOST_REQUIRE(request.available());
        BOOST_CHECK_EQUAL(request.get(), request_size);
    }
}

SEASTAR_THREAD_TEST_CASE(test_pending_io_has_no_wakeup_after_cancellation) {
    internal::disk_config_params disk_config(1);
    internal::disk_params disk;
    disk.read_bytes_rate = 1ul << 30;
    disk.write_bytes_rate = 1ul << 30;
    disk.read_req_rate = 1000;
    disk.write_req_rate = 1000;
    auto io_config = disk_config.generate_config(disk, 0, 1);
    io_config.mountpoint = "pending-cancelled-request";
    io_config.rate_limit_duration = std::chrono::milliseconds(100);
    io_queue_for_tests tio(io_config);
    tio.kicker.cancel();

    io_intent intent;
    auto request = tio.queue_request(get_default_pc(),
            internal::io_direction_and_length(internal::io_direction_and_length::write_idx, 4096),
            internal::io_request::make_write(0, 0, nullptr, 4096, false), &intent, {});
    tio.reserve_queued_capacity();
    intent.cancel();
    tio.queue.poll_io_queue();

    BOOST_REQUIRE(tio.has_pending_reservation());
    BOOST_REQUIRE(tio.get_fair_queue().top() == nullptr);
    BOOST_CHECK(tio.next_pending_aio() == io_queue::clock_type::time_point::max());
    BOOST_CHECK_THROW(request.get(), cancelled_error);
}

enum class part_flaw { none, partial, error };

static void do_test_large_request_flow(part_flaw flaw) {
    io_queue_for_tests tio;
    fake_file file;
    int values[3] = { 13, 42, 73 };

    auto limits = tio.queue.get_request_limits();

    std::vector<::iovec> vecs;
    auto f = tio.queue_request(get_default_pc(), internal::io_direction_and_length(internal::io_direction_and_length::write_idx, limits.max_write * 3),
                    file.make_writev_req(0, values, 3, limits.max_write, vecs), nullptr, std::move(vecs))
    .then([&file, &values, &limits, flaw] (size_t len) {
        size_t expected = limits.max_write;

        BOOST_REQUIRE_EQUAL(file.data[0 * limits.max_write], values[0]);

        if (flaw == part_flaw::none) {
            BOOST_REQUIRE_EQUAL(file.data[1 * limits.max_write], values[1]);
            BOOST_REQUIRE_EQUAL(file.data[2 * limits.max_write], values[2]);
            expected += 2 * limits.max_write;
        }

        if (flaw == part_flaw::partial) {
            BOOST_REQUIRE_EQUAL(file.data[1 * limits.max_write], values[1]);
            expected += limits.max_write / 2;
        }

        BOOST_REQUIRE_EQUAL(len, expected);
    });

    for (int i = 0; i < 3; i++) {
        seastar::sleep(std::chrono::milliseconds(500)).get();
        tio.queue.poll_io_queue();
        tio.sink.drain([&file, i, flaw] (const internal::io_request& rq, io_completion* desc) -> bool {
            if (i == 1) {
                if (flaw == part_flaw::partial) {
                    const auto& op = rq.as<internal::io_request::operation::writev>();
                    op.iovec[0].iov_len /= 2;
                }
                if (flaw == part_flaw::error) {
                    desc->complete_with(-EIO);
                    return true;
                }
            }
            file.execute_writev_req(rq, desc);
            return true;
        });
    }

    f.get();
}

SEASTAR_THREAD_TEST_CASE(test_large_request_flow) {
    do_test_large_request_flow(part_flaw::none);
}

SEASTAR_THREAD_TEST_CASE(test_large_request_flow_partial) {
    do_test_large_request_flow(part_flaw::partial);
}

SEASTAR_THREAD_TEST_CASE(test_large_request_flow_error) {
    do_test_large_request_flow(part_flaw::error);
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
    fake_file file;

    auto sg0 = create_scheduling_group("a", 100).get();
    auto sg1 = create_scheduling_group("b", 100).get();
    auto stop = defer([&] () noexcept {
        destroy_scheduling_group(sg0).get();
        destroy_scheduling_group(sg1).get();
    });

    io_queue_for_tests tio;

    size_t idx = 0;
    int val = 100;

    io_intent live, dead;

    std::vector<future<>> finished;
    std::vector<future<>> cancelled;

    auto queue_legacy_request = [&] (io_queue_for_tests& q, scheduling_group sg) {
        auto buf = std::make_unique<int>(val);
        auto f = q.queue_request(sg, internal::io_direction_and_length(internal::io_direction_and_length::write_idx, 0), file.make_write_req(idx, buf.get()), nullptr, {})
            .then([&file, idx, val, buf = std::move(buf)] (size_t len) {
                BOOST_REQUIRE(file.data[idx] == val);
                return make_ready_future<>();
            });
        finished.push_back(std::move(f));
        idx++;
        val++;
    };

    auto queue_live_request = [&] (io_queue_for_tests& q, scheduling_group sg) {
        auto buf = std::make_unique<int>(val);
        auto f = q.queue_request(sg, internal::io_direction_and_length(internal::io_direction_and_length::write_idx, 0), file.make_write_req(idx, buf.get()), &live, {})
            .then([&file, idx, val, buf = std::move(buf)] (size_t len) {
                BOOST_REQUIRE(file.data[idx] == val);
                return make_ready_future<>();
            });
        finished.push_back(std::move(f));
        idx++;
        val++;
    };

    auto queue_dead_request = [&] (io_queue_for_tests& q, scheduling_group sg) {
        auto buf = std::make_unique<int>(val);
        auto f = q.queue_request(sg, internal::io_direction_and_length(internal::io_direction_and_length::write_idx, 0), file.make_write_req(idx, buf.get()), &dead, {})
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
            queue_live_request(tio, pc == 0 ? sg0 : sg1);
        } else if (dice(reng) < 5) {
            fmt::print("queue dead req to pc {}\n", pc);
            queue_dead_request(tio, pc == 0 ? sg0 : sg1);
        } else {
            fmt::print("queue legacy req to pc {}\n", pc);
            queue_legacy_request(tio, pc == 0 ? sg0 : sg1);
        }
    }

    dead.cancel();

    // cancelled requests must resolve right at once

    when_all_succeed(cancelled.begin(), cancelled.end()).get();

    seastar::sleep(std::chrono::milliseconds(500)).get();
    tio.queue.poll_io_queue();
    tio.sink.drain([&file] (const internal::io_request& rq, io_completion* desc) -> bool {
        file.execute_write_req(rq, desc);
        return true;
    });

    when_all_succeed(finished.begin(), finished.end()).get();
}

SEASTAR_TEST_CASE(test_request_buffer_split) {
    auto ensure = [] (const std::vector<internal::io_request::part>& parts, const internal::io_request& req, int idx, uint64_t pos, size_t size, uintptr_t mem) {
        BOOST_REQUIRE(parts[idx].req.opcode() == req.opcode());
        const auto& op = req.as<internal::io_request::operation::read>();
        const auto& sub_op = parts[idx].req.as<internal::io_request::operation::read>();
        BOOST_REQUIRE_EQUAL(sub_op.fd, op.fd);
        BOOST_REQUIRE_EQUAL(sub_op.pos, pos);
        BOOST_REQUIRE_EQUAL(sub_op.size, size);
        BOOST_REQUIRE_EQUAL(sub_op.addr, reinterpret_cast<void*>(mem));
        BOOST_REQUIRE_EQUAL(sub_op.nowait_works, op.nowait_works);
        BOOST_REQUIRE_EQUAL(parts[idx].iovecs.size(), 0);
        BOOST_REQUIRE_EQUAL(parts[idx].size, sub_op.size);
    };

    // No split
    {
        internal::io_request req = internal::io_request::make_read(5, 13, reinterpret_cast<void*>(0x420), 17, true);
        auto parts = req.split(21);
        BOOST_REQUIRE_EQUAL(parts.size(), 1);
        ensure(parts, req, 0, 13, 17, 0x420);
    }

    // Without tail
    {
        internal::io_request req = internal::io_request::make_read(7, 24, reinterpret_cast<void*>(0x4321), 24, true);
        auto parts = req.split(12);
        BOOST_REQUIRE_EQUAL(parts.size(), 2);
        ensure(parts, req, 0, 24,      12, 0x4321);
        ensure(parts, req, 1, 24 + 12, 12, 0x4321 + 12);
    }

    // With tail
    {
        internal::io_request req = internal::io_request::make_read(9, 42, reinterpret_cast<void*>(0x1234), 33, true);
        auto parts = req.split(13);
        BOOST_REQUIRE_EQUAL(parts.size(), 3);
        ensure(parts, req, 0, 42,      13, 0x1234);
        ensure(parts, req, 1, 42 + 13, 13, 0x1234 + 13);
        ensure(parts, req, 2, 42 + 26,  7, 0x1234 + 26);
    }

    return make_ready_future<>();
}

static void show_request(const internal::io_request& req, void* buf_off, std::string pfx = "") {
    if (!seastar_logger.is_enabled(log_level::trace)) {
        return;
    }

    const auto& op = req.as<internal::io_request::operation::readv>();
    seastar_logger.trace("{}{} iovecs on req:", pfx, op.iov_len);
    for (unsigned i = 0; i < op.iov_len; i++) {
        seastar_logger.trace("{}  base={} len={}", pfx, reinterpret_cast<uintptr_t>(op.iovec[i].iov_base) - reinterpret_cast<uintptr_t>(buf_off), op.iovec[i].iov_len);
    }
}

static void show_request_parts(const std::vector<internal::io_request::part>& parts, void* buf_off) {
    if (!seastar_logger.is_enabled(log_level::trace)) {
        return;
    }

    seastar_logger.trace("{} parts", parts.size());
    for (const auto& p : parts) {
        seastar_logger.trace("  size={} iovecs={}", p.size, p.iovecs.size());
        seastar_logger.trace("  {} iovecs on part:", p.iovecs.size());
        for (const auto& iov : p.iovecs) {
            seastar_logger.trace("    base={} len={}", reinterpret_cast<uintptr_t>(iov.iov_base) - reinterpret_cast<uintptr_t>(buf_off), iov.iov_len);
        }
        show_request(p.req, buf_off, "  ");
    }
}

SEASTAR_TEST_CASE(test_request_iovec_split) {
    char large_buffer[1025];

    auto clear_buffer = [&large_buffer] {
        memset(large_buffer, 0, sizeof(large_buffer));
    };

    auto bump_buffer = [] (const std::vector<::iovec>& vecs) {
        for (auto&& v : vecs) {
            for (unsigned i = 0; i < v.iov_len; i++) {
                (reinterpret_cast<char*>(v.iov_base))[i]++;
            }
        }
    };

    auto check_buffer = [&large_buffer] (size_t len, char value) {
        SEASTAR_ASSERT(len < sizeof(large_buffer));
        bool fill_match = true;
        bool train_match = true;
        for (unsigned i = 0; i < sizeof(large_buffer); i++) {
            if (i < len) {
                if (large_buffer[i] != value) {
                    fill_match = false;
                }
            } else {
                if (large_buffer[i] != '\0') {
                    train_match = false;
                }
            }
        }
        BOOST_REQUIRE_EQUAL(fill_match, true);
        BOOST_REQUIRE_EQUAL(train_match, true);
    };

    auto ensure = [] (const std::vector<internal::io_request::part>& parts, const internal::io_request& req, int idx, uint64_t pos) {
        BOOST_REQUIRE(parts[idx].req.opcode() == req.opcode());
        const auto& op = req.as<internal::io_request::operation::writev>();
        const auto& sub_op = parts[idx].req.as<internal::io_request::operation::writev>();
        BOOST_REQUIRE_EQUAL(sub_op.fd, op.fd);
        BOOST_REQUIRE_EQUAL(sub_op.pos, pos);
        BOOST_REQUIRE_EQUAL(sub_op.iov_len, parts[idx].iovecs.size());
        BOOST_REQUIRE_EQUAL(sub_op.nowait_works, op.nowait_works);
        BOOST_REQUIRE_EQUAL(parts[idx].size, internal::iovec_len(parts[idx].iovecs));

        for (unsigned iov = 0; iov < parts[idx].iovecs.size(); iov++) {
            BOOST_REQUIRE_EQUAL(sub_op.iovec[iov].iov_base, parts[idx].iovecs[iov].iov_base);
            BOOST_REQUIRE_EQUAL(sub_op.iovec[iov].iov_len, parts[idx].iovecs[iov].iov_len);
        }
    };

    std::default_random_engine& reng = testing::local_random_engine;
    auto dice = std::uniform_int_distribution<uint16_t>(1, 31);
    auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    uint64_t iter = 0;
    unsigned no_splits = 0;
    unsigned no_tails = 0;

    do {
        seastar_logger.debug("===== iter {} =====", iter++);
        std::vector<::iovec> vecs;
        unsigned nr_vecs = dice(reng) % 13 + 1;
        seastar_logger.debug("Generate {} iovecs", nr_vecs);
        size_t total = 0;
        for (unsigned i = 0; i < nr_vecs; i++) {
            ::iovec iov;
            iov.iov_base = reinterpret_cast<void*>(large_buffer + total);
            iov.iov_len = dice(reng);
            SEASTAR_ASSERT(iov.iov_len != 0);
            total += iov.iov_len;
            vecs.push_back(std::move(iov));
        }

        SEASTAR_ASSERT(total > 0);
        clear_buffer();
        bump_buffer(vecs);
        check_buffer(total, 1);

        size_t file_off = dice(reng);
        internal::io_request req = internal::io_request::make_readv(5, file_off, vecs, true);

        show_request(req, large_buffer);

        size_t max_len = dice(reng) * 3;
        unsigned nr_parts = (total + max_len - 1) / max_len;
        seastar_logger.debug("Split {} into {}-bytes ({} parts)", total, max_len, nr_parts);
        auto parts = req.split(max_len);
        show_request_parts(parts, large_buffer);
        BOOST_REQUIRE_EQUAL(parts.size(), nr_parts);

        size_t parts_total = 0;
        for (unsigned p = 0; p < nr_parts; p++) {
            ensure(parts, req, p, file_off + parts_total);
            if (p < nr_parts - 1) {
                BOOST_REQUIRE_EQUAL(parts[p].size, max_len);
            }
            parts_total += parts[p].size;
            bump_buffer(parts[p].iovecs);
        }
        BOOST_REQUIRE_EQUAL(parts_total, total);
        check_buffer(total, 2);

        if (parts.size() == 1) {
            no_splits++;
        }
        if (parts.back().size == max_len) {
            no_tails++;
        }
    } while (std::chrono::steady_clock::now() < stop || iter < 32 || no_splits < 16 || no_tails < 16);

    seastar_logger.info("{} iters ({} no-splits, {} no-tails)", iter, no_splits, no_tails);

    return make_ready_future<>();
}

SEASTAR_THREAD_TEST_CASE(test_tb_params) {
    internal::disk_config_params disk_config(1);
    internal::disk_params d;
    size_t iops_req_size = 512;
    size_t bw_req_size = 128 << 10; // 128k

    // Test multipl datapoints starting at 1GB/s for read/write bandwidth
    // and 15k req/s for read/write IOPS
    for (uint64_t i = 1; i < 65; ++i) {
        d.read_bytes_rate = i << 30;  // iGB/s
        d.write_bytes_rate = i << 30;
        d.read_req_rate = i * 15000;
        d.write_req_rate = i * 15000;

        auto io_config = disk_config.generate_config(d, 0, 1);
        io_config.mountpoint = std::to_string(i);

        io_queue_for_tests tio(io_config);

        auto cost_read_512 = tio.queue.request_capacity(internal::io_direction_and_length(internal::io_direction_and_length::read_idx, iops_req_size));
        auto cost_write_512 = tio.queue.request_capacity(internal::io_direction_and_length(internal::io_direction_and_length::write_idx, iops_req_size));

        auto cost_read_128k = tio.queue.request_capacity(internal::io_direction_and_length(internal::io_direction_and_length::read_idx, bw_req_size));
        auto cost_write_128k = tio.queue.request_capacity(internal::io_direction_and_length(internal::io_direction_and_length::write_idx, bw_req_size));
        seastar_logger.info("{} read req/s, {} write req/s, {} read bytes/s, {} write bytes/s",
                            d.read_req_rate, d.write_req_rate, d.read_bytes_rate, d.write_bytes_rate);

        seastar_logger.info("Read 512 cost: {}, Write 512 cost: {}", cost_read_512, cost_write_512);
        seastar_logger.info("Read 128k cost: {}, Write 128k cost: {}", cost_read_128k, cost_write_128k);

        const auto& fg = internal::get_throttler(tio.queue, internal::io_direction_and_length::write_idx);
        const auto& tb = fg.token_bucket();
        double rate = tb.rate();

        auto fg_rate = std::chrono::duration<double, io_throttler::rate_resolution>(std::chrono::seconds(1)).count();
        double iops_read = rate / cost_read_512 * fg_rate;
        double iops_write = rate / cost_write_512 * fg_rate;
        double bandwidth_read = rate / (cost_read_128k - cost_read_512) * (bw_req_size - iops_req_size) * fg_rate;
        double bandwidth_write = rate / (cost_write_128k - cost_write_512) * (bw_req_size - iops_req_size) * fg_rate;
        seastar_logger.info("IOPS read: {}, IOPS write: {}, Bandwidth read: {}, Bandwidth write: {}",
                            iops_read, iops_write, bandwidth_read, bandwidth_write);

        auto within_margin = [&] (double computed, double configured) {
            float error_margin = 0.05f; // 5% error margin
            return std::abs(computed - configured) / configured < error_margin;
        };
        BOOST_CHECK(within_margin(iops_read, d.read_req_rate));
        BOOST_CHECK(within_margin(iops_write, d.write_req_rate));
        BOOST_CHECK(within_margin(bandwidth_read, d.read_bytes_rate));
        BOOST_CHECK(within_margin(bandwidth_write, d.write_bytes_rate));
    }
}

SEASTAR_THREAD_TEST_CASE(test_unconfigured_io_queue) {
    io_queue_for_tests tio;

    BOOST_CHECK_EQUAL(tio.max_request_length(internal::io_direction_and_length::read_idx), tio.request_length_limit());
    BOOST_CHECK_EQUAL(tio.max_request_length(internal::io_direction_and_length::write_idx), tio.request_length_limit());

    for (uint64_t reqsize = 512; reqsize < 128 << 10; reqsize <<= 1) {
        auto cost_read = tio.queue.request_capacity(internal::io_direction_and_length(internal::io_direction_and_length::read_idx, reqsize));
        auto cost_write = tio.queue.request_capacity(internal::io_direction_and_length(internal::io_direction_and_length::write_idx, reqsize));

        BOOST_CHECK_EQUAL(cost_read, 0);
        BOOST_CHECK_EQUAL(cost_write, 0);
    }
}

namespace seastar::testing {
class fair_queue_test {
    fair_queue& _fq;
public:
    fair_queue_test(fair_queue& fq) noexcept : _fq(fq) {}

    unsigned nr_children_for(std::optional<unsigned> index) {
        return index.has_value() ? _fq._priority_groups[*index]->_nr_children : _fq._root._nr_children;
    }

    bool is_root_group(unsigned index) {
        return _fq._priority_groups[index]->_parent == &_fq._root;
    }

    unsigned group_shares(unsigned index) {
        return _fq._priority_groups[index]->_shares;
    }

    std::optional<unsigned> get_parent_index(scheduling_group sg) {
        auto& pe = reinterpret_cast<fair_queue::priority_entry&>(*_fq._priority_classes[internal::scheduling_group_index(sg)]);
        if (pe._parent == &_fq._root) {
            return {};
        }

        unsigned i = 0;
        for (auto& pg : _fq._priority_groups) {
            if (pe._parent == pg.get()) {
                break;
            }
            i++;
        }
        return i;
    }
};
}

SEASTAR_THREAD_TEST_CASE(test_nested_priority_classes_basic_linkage) {
    io_queue_for_tests tio;

    auto ssg1 = create_scheduling_supergroup(100).get();
    auto ssg2 = create_scheduling_supergroup(200).get();
    auto sg0 = create_scheduling_group("a", 300).get();
    auto sg1 = create_scheduling_group("b", "b", 400, ssg1).get();
    auto sg2 = create_scheduling_group("c", "c", 500, ssg2).get();
    auto stop = defer([&] () noexcept {
        destroy_scheduling_group(sg0).get();
        destroy_scheduling_group(sg1).get();
        destroy_scheduling_group(sg2).get();
    });

    tio.find_or_create_class(sg0);
    tio.find_or_create_class(sg1);
    tio.find_or_create_class(sg2);

    seastar::testing::fair_queue_test fq(tio.get_fair_queue());

    BOOST_CHECK_EQUAL(fq.nr_children_for({}), 3);
    BOOST_CHECK_EQUAL(fq.nr_children_for(0), 1);
    BOOST_CHECK_EQUAL(fq.nr_children_for(1), 1);

    BOOST_CHECK(fq.is_root_group(0));
    BOOST_CHECK(fq.is_root_group(1));

    BOOST_CHECK(!fq.get_parent_index(sg0));
    BOOST_CHECK(fq.get_parent_index(sg1) == 0);
    BOOST_CHECK(fq.get_parent_index(sg2) == 1);
}

SEASTAR_THREAD_TEST_CASE(test_supergroup_index_reuse_refreshes_group_shares) {
    io_queue_for_tests tio;

    auto ssg = create_scheduling_supergroup(100).get();
    auto sg = create_scheduling_group("a", "a", 100, ssg).get();

    tio.find_or_create_class(sg);

    seastar::testing::fair_queue_test fq(tio.get_fair_queue());
    BOOST_REQUIRE_EQUAL(fq.group_shares(ssg.index()), 100);

    tio.queue.destroy_priority_class(sg);
    destroy_scheduling_group(sg).get();
    auto reused_index = ssg.index();
    destroy_scheduling_supergroup(ssg).get();

    // Supergroups are allocated from the first free slot, so the new one
    // takes over the index of the one just destroyed. The io-queue keeps
    // the group data of the old supergroup on that index, and must still
    // apply the new supergroup's shares to the fair-queue group
    auto ssg2 = create_scheduling_supergroup(800).get();
    BOOST_REQUIRE_EQUAL(ssg2.index(), reused_index);
    auto sg2 = create_scheduling_group("b", "b", 100, ssg2).get();

    tio.find_or_create_class(sg2);
    BOOST_REQUIRE_EQUAL(fq.group_shares(ssg2.index()), 800);

    tio.queue.destroy_priority_class(sg2);
    destroy_scheduling_group(sg2).get();
    destroy_scheduling_supergroup(ssg2).get();
}

SEASTAR_THREAD_TEST_CASE(test_destroy_priority_class_with_requests) {
    io_queue_for_tests tio;

    auto sg = create_scheduling_group("a", 100).get();

    auto fx = tio.queue_request(sg,
        internal::io_direction_and_length(internal::io_direction_and_length::read_idx, 0),
        internal::io_request::make_write(0, 0, nullptr, 1, false),
        nullptr, {});

    // Push this request through to make io-queue instantiate the priority class
    tio.queue.poll_io_queue();
    tio.sink.drain([] (const internal::io_request& rq, io_completion* desc) -> bool {
        desc->complete_with(1);
        return true;
    });
    BOOST_REQUIRE_EQUAL(fx.get(), 1);

    BOOST_REQUIRE(tio.is_class_registered(sg));
    tio.queue.destroy_priority_class(sg);
    destroy_scheduling_group(sg).get();
    BOOST_REQUIRE(!tio.is_class_registered(sg));
}

SEASTAR_THREAD_TEST_CASE(test_gauge_integrator_test) {
    util::integrated_length<unsigned short, std::chrono::steady_clock> gi;
    auto seed = std::random_device{}();
    std::default_random_engine reng(seed);
    std::uniform_int_distribution<> qlen(0, 250);
    std::uniform_int_distribution<> dur(100, 1000);
    auto now = std::chrono::steady_clock::now();
    auto prev_value = gi.integral();

    auto accumulate = [&] (std::function<unsigned short()> v) {
        std::chrono::microseconds total_duration(0);
        for (int i = 0; i < 13425; i++) {
            auto d = std::chrono::microseconds(dur(reng));
            total_duration += d;
            now += d;
            gi = v();
            gi.checkpoint(now);
        }
        return std::chrono::duration_cast<std::chrono::seconds>(total_duration);
    };

    // step one -- check static value
    for (int m = 0; m < 10; m++) {
        auto value = qlen(reng);
        auto seconds = accumulate([value] { return value; });
        auto iv = gi.integral();
        auto delta = iv - prev_value;
        prev_value = iv;
        fmt::print("value={:<6d} integral={:<10d} duration={:<4d} result={:<6d}\n",
            value, delta, seconds.count(), delta / seconds.count()
        );
        BOOST_REQUIRE_GE(delta / seconds.count(), (unsigned short)(value * 0.9));
        BOOST_REQUIRE_LE(delta / seconds.count(), (unsigned short)(value * 1.9));
    }

    // step two -- check disperse values
    for (int m = 0; m < 16; m++) {
        auto min_v = std::numeric_limits<unsigned short>::max();
        auto max_v = std::numeric_limits<unsigned short>::min();
        auto seconds = accumulate([&] {
                auto v = qlen(reng);
                min_v = std::min<unsigned short>(v, min_v);
                max_v = std::max<unsigned short>(v, max_v);
                return v;
        });
        auto iv = gi.integral();
        auto delta = iv - prev_value;
        prev_value = iv;
        fmt::print("value=[{:d},{:d}] integral={:<10d} duration={:<4d} result={:<6d}\n",
            min_v, max_v, delta, seconds.count(), delta / seconds.count()
        );
        BOOST_REQUIRE_GE(delta / seconds.count(), min_v);
        BOOST_REQUIRE_LE(delta / seconds.count(), max_v);
    }
    fmt::print("done\n");
}

// The token bucket batches replenishments in bandwidth_threshold_in_blocks
// (128 KiB) increments; the last batch in a measurement window may overshoot
// the configured bandwidth+burst limit by up to one threshold unit.
static constexpr size_t bw_slack = 128*1024;

struct bandwidth_measurement {
    size_t bytes = 0;
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point end;

    size_t bandwidth() const {
        return bytes / std::chrono::duration<double>(end - start).count();
    }
};

// Bandwidth of both workloads over one window that spans both runs. Adding
// their separate bandwidths overstates it: the workload measured later has the
// other's share once the other stops, but each is divided by its own window.
static size_t combined_bandwidth(const bandwidth_measurement& a, const bandwidth_measurement& b) {
    return bandwidth_measurement{a.bytes + b.bytes, std::min(a.start, b.start), std::max(a.end, b.end)}.bandwidth();
}

// The measured bandwidth is a cumulative average since the start of the run, so
// the initial burst is folded into it: over a one second window the burst
// accounts for ~9% of the bytes seen, and it shrinks as 1/window from there. A
// caller that compares two of these against each other wants a window long
// enough that the burst no longer dominates the comparison; min_window puts a
// floor on the measured interval for those. The default preserves the
// historical behaviour of returning on the first sample that meets the goal.
static future<bandwidth_measurement> run_and_check_bandwidth(io_queue_for_tests& tio, scheduling_group sg, size_t bandwidth_goal, unsigned parallelizm = 1, std::chrono::seconds min_window = std::chrono::seconds(1), size_t req_size = 128*1024) {
    fmt::print("Run {} workload\n", internal::scheduling_group_index(sg));
    bool keep_going = true;
    uint64_t nr_requests = 0;

    auto submitter = parallel_for_each(std::views::iota(0u, parallelizm), [&] (auto i) {
        return do_until([&keep_going] { return !keep_going; }, [&] {
            return tio.queue_request(sg,
                internal::io_direction_and_length(internal::io_direction_and_length::read_idx, req_size),
                internal::io_request::make_write(0, 0, nullptr, req_size, false),
                nullptr, {}).then([&nr_requests] (auto size) {
                    nr_requests++;
                    return make_ready_future<>();
                });
        });
    });


    auto start = std::chrono::steady_clock::now();
    auto stop = start + std::chrono::seconds(60);
    bandwidth_measurement result;

    while (true) {
        co_await seastar::sleep(std::chrono::seconds(1));
        auto now = std::chrono::steady_clock::now();
        result = {nr_requests * req_size, start, now};
        auto real_bandwidth = result.bandwidth();
        fmt::print("Measured for {} {} MB/s, goal {} MB/s\n", internal::scheduling_group_index(sg), real_bandwidth >> 20, bandwidth_goal >> 20);
        if ((real_bandwidth >= bandwidth_goal && now >= start + min_window) || now >= stop) {
            break;
        }
    }

    keep_going = false;
    co_await std::move(submitter);
    co_return result;
}

struct background_drain {
    io_queue_for_tests& tio;
    bool keep_going;
    future<> done;

    future<> start_draining() {
        return async([this] {
            while (keep_going) {
                tio.queue.poll_io_queue();
                tio.sink.drain([] (const internal::io_request& rq, io_completion* desc) -> bool {
                    const auto& op = rq.as<internal::io_request::operation::write>();
                    desc->complete_with(op.size);
                    return true;
                });
                maybe_yield().get();
            }
        });
    }

    background_drain(io_queue_for_tests& t)
        : tio(t)
        , keep_going(true)
        , done(start_draining())
    { }

    future<> stop() {
        fmt::print("Stop drainer\n");
        keep_going = false;
        return std::move(done);
    }
};

SEASTAR_THREAD_TEST_CASE(test_class_bandwidth_throttler) {
    io_queue_for_tests tio;

    const size_t bandwidth = 100*1024*1024;

    auto sg = create_scheduling_group("a", 100).get();
    auto destroy_groups = defer([&] () noexcept {
        destroy_scheduling_group(sg).get();
    });
    tio.queue.update_bandwidth_for_class(sg, bandwidth).get();

    background_drain drain(tio);
    auto stop = defer([&] () noexcept {
        drain.stop().get();
    });

    auto bw = run_and_check_bandwidth(tio, sg, bandwidth * 0.9).get().bandwidth();
    BOOST_REQUIRE_LE(bw, bandwidth * 1.15);
}

SEASTAR_THREAD_TEST_CASE(test_class_group_bandwidth_throttler) {
    io_queue_for_tests tio;

    const size_t burst = 10*1024*1024;
    const size_t bandwidth = 100*1024*1024;
    auto ssg = create_scheduling_supergroup(100).get();
    auto sg = create_scheduling_group("a", "a", 100, ssg).get();
    auto destroy_groups = defer([&] () noexcept {
        destroy_scheduling_group(sg).get();
        destroy_scheduling_supergroup(ssg).get();
    });
    tio.queue.update_bandwidth_for_class_group(ssg, bandwidth).get();

    background_drain drain(tio);
    auto stop = defer([&] () noexcept {
        drain.stop().get();
    });

    auto bw = run_and_check_bandwidth(tio, sg, bandwidth * 0.9).get().bandwidth();
    BOOST_REQUIRE_LE(bw, bandwidth + burst + bw_slack);
}

SEASTAR_THREAD_TEST_CASE(test_2_class_group_bandwidth_throttler) {
    io_queue_for_tests tio;

    const size_t burst = 10*1024*1024;
    const size_t bandwidth = 70*1024*1024;
    const size_t group_bandwidth = 100*1024*1024;

    auto ssg = create_scheduling_supergroup(100).get();
    auto sg0 = create_scheduling_group("a", "a", 100, ssg).get();
    auto sg1 = create_scheduling_group("b", "b", 100, ssg).get();
    auto destroy_groups = defer([&] () noexcept {
        destroy_scheduling_group(sg1).get();
        destroy_scheduling_group(sg0).get();
        destroy_scheduling_supergroup(ssg).get();
    });

    tio.queue.update_bandwidth_for_class(sg0, bandwidth).get();
    tio.queue.update_bandwidth_for_class(sg1, bandwidth).get();
    tio.queue.update_bandwidth_for_class_group(ssg, group_bandwidth).get();

    background_drain drain(tio);
    auto stop = defer([&] () noexcept {
        drain.stop().get();
    });

    // Set goal to be 40% of the maximum, as both classes will hit
    // the group limit and won't reach their personal limits
    auto f0 = run_and_check_bandwidth(tio, sg0, bandwidth * 0.4);
    auto f1 = run_and_check_bandwidth(tio, sg1, bandwidth * 0.4);

    auto m0 = f0.get();
    auto m1 = f1.get();

    // None of the classes must exceed its personal bandwidth
    BOOST_REQUIRE_LE(m0.bandwidth(), bandwidth + burst + bw_slack);
    BOOST_REQUIRE_LE(m1.bandwidth(), bandwidth + burst + bw_slack);
    // Both classes must not exceed the group bandwidth
    BOOST_REQUIRE_LE(combined_bandwidth(m0, m1), group_bandwidth + burst + bw_slack);
}

SEASTAR_THREAD_TEST_CASE(test_2_class_group_bandwidth_throttler_1_unlimited) {
    io_queue_for_tests tio;

    const size_t burst = 10*1024*1024;
    const size_t bandwidth = 40*1024*1024;
    const size_t group_bandwidth = 100*1024*1024;

    auto ssg = create_scheduling_supergroup(100).get();
    auto sg0 = create_scheduling_group("a", "a", 100, ssg).get();
    auto sg1 = create_scheduling_group("b", "b", 100, ssg).get();
    auto destroy_groups = defer([&] () noexcept {
        destroy_scheduling_group(sg1).get();
        destroy_scheduling_group(sg0).get();
        destroy_scheduling_supergroup(ssg).get();
    });

    tio.queue.update_bandwidth_for_class(sg0, bandwidth).get();
    tio.queue.update_bandwidth_for_class_group(ssg, group_bandwidth).get();

    background_drain drain(tio);
    auto stop = defer([&] () noexcept {
        drain.stop().get();
    });

    // Set goal to be 40% of the maximum, as both classes will hit
    // the group limit and won't reach their personal limits
    auto f0 = run_and_check_bandwidth(tio, sg0, bandwidth * 0.4);
    auto f1 = run_and_check_bandwidth(tio, sg1, group_bandwidth * 0.4);

    auto m0 = f0.get();
    auto m1 = f1.get();

    // Limited class must not exceed its personal bandwidth
    BOOST_REQUIRE_LE(m0.bandwidth(), bandwidth + burst + bw_slack);
    // Both classes must not exceed the group bandwidth
    BOOST_REQUIRE_LE(combined_bandwidth(m0, m1), group_bandwidth + burst + bw_slack);
}

SEASTAR_THREAD_TEST_CASE(test_2_class_group_bandwidth_throttler_fair_shares) {
    const size_t burst = 10*1024*1024;
    const size_t bandwidth = 100*1024*1024;

    io_queue::config cfg{0};
    cfg.blocks_count_rate = io_queue::read_request_base_count * ((bandwidth * 10) >> io_queue::block_size_shift);
    io_queue_for_tests tio(cfg);

    auto ssg = create_scheduling_supergroup(200).get();
    auto sg0 = create_scheduling_group("a", "a", 400, ssg).get();
    auto sg1 = create_scheduling_group("b", "b", 100, ssg).get();
    auto destroy_groups = defer([&] () noexcept {
        destroy_scheduling_group(sg1).get();
        destroy_scheduling_group(sg0).get();
        destroy_scheduling_supergroup(ssg).get();
    });

    tio.queue.update_bandwidth_for_class_group(ssg, bandwidth).get();

    background_drain drain(tio);
    auto stop = defer([&] () noexcept {
        drain.stop().get();
    });

    // Unlike the tests above, this one compares the two bandwidths against each
    // other, within 1.25%. A one second window cannot support that: the burst is
    // then ~9% of what is measured, so a couple of requests' worth of variation
    // in how it lands between the two classes moves the ratio further than the
    // check allows. Four seconds dilutes the burst term, and the quantisation of
    // the measurement itself, by 4x.
    auto f0 = run_and_check_bandwidth(tio, sg0, bandwidth * 0.8 * 0.95, 4, std::chrono::seconds(4));
    auto f1 = run_and_check_bandwidth(tio, sg1, bandwidth * 0.2 * 0.95, 4, std::chrono::seconds(4));
    auto m0 = f0.get();
    auto m1 = f1.get();
    auto bw0 = m0.bandwidth();
    auto bw1 = m1.bandwidth();

    // Check that shares are roughly respected
    BOOST_REQUIRE_LE(float(bw0) / float(bw1), 4.05);
    BOOST_REQUIRE_GE(float(bw0) / float(bw1), 3.95);
    BOOST_REQUIRE_LE(combined_bandwidth(m0, m1), bandwidth + burst + bw_slack);
}

static io_queue::config rate_correction_config(uint64_t bytes_rate = 1 << 30) {
    internal::disk_config_params disk_config(1);
    internal::disk_params disk;
    disk.read_bytes_rate = disk.write_bytes_rate = bytes_rate;
    disk.read_req_rate = disk.write_req_rate = 100000;
    auto cfg = disk_config.generate_config(disk, 0, 1);
    cfg.disk_latency_threshold = std::chrono::milliseconds(5);
    cfg.with_metrics = false;
    return cfg;
}

// The start of the next period: the next whole second of the steady clock
static io_queue::clock_type::time_point next_period_start() {
    return io_queue::clock_type::time_point(std::chrono::ceil<std::chrono::seconds>(io_queue::clock_type::now().time_since_epoch()));
}

// A load of writes on a drive that serves one write at a time
struct drive_load {
    // The fraction of the configured rates that the drive delivers. If `later`
    // is not 0, the drive delivers `performance_later` from second `later`.
    double performance;
    double performance_later = 0;
    int later = 0;
    // The number of writes that the load keeps in the queue and in the drive,
    // and their size. Size 0 is the largest write that the queue sends.
    unsigned writes = 128;
    size_t write_size = 128 << 10;
    int seconds = 1200;
};

// c and the average time of the writes in the drive, for one second
struct corrected_second {
    double correction;
    double in_disk;
};

// Simulates a load on a drive, event by event. The queue gives the cost of the
// writes. The bucket lets the writes through at c times the configured rate.
// The drive serves the writes one at a time. After each simulated second, the
// queue gets the measurements of the second, and the regulator runs. Returns c
// and the average time in the drive for each second.
static std::vector<corrected_second> simulate_rate_correction(const drive_load& load) {
    io_queue_for_tests tio(rate_correction_config());
    const auto& bucket = internal::get_throttler(tio.queue, 0);
    auto dnl = internal::io_direction_and_length(internal::io_direction_and_length::write_idx,
            load.write_size ? load.write_size : tio.queue.get_request_limits().max_write);
    // The cost of a write and the limit of the bucket, in seconds of the
    // configured rate
    const double cost = io_throttler::capacity_tokens(tio.queue.request_capacity(dnl));
    const double limit = io_throttler::capacity_tokens(bucket.maximum_capacity());
    // The number of writes in the queue, and for each write in the drive, its
    // dispatch time and its completion time
    unsigned waiting = load.writes;
    std::deque<std::pair<double, double>> in_drive;
    double now = 0, tokens = 0, free_at = 0, in_disk = 0, occupancy = 0, sent = 0;
    uint64_t completed = 0;
    std::vector<corrected_second> result;
    auto start = next_period_start();
    for (int second = 1; second <= load.seconds; second++) {
        const double service = cost / (load.later && second > load.later ? load.performance_later : load.performance);
        // The bucket fills at c seconds of the configured rate per second
        const double fill = double(bucket.token_bucket().rate()) / io_throttler::fixed_point_factor;
        for (;;) {
            double done = in_drive.empty() ? second : in_drive.front().second;
            double ready = waiting ? now + std::max(cost - tokens, 0.0) / fill : second;
            double next = std::min({done, ready, double(second)});
            // The writes in the drive, for the time until the next event
            occupancy += in_drive.size() * (next - now);
            tokens = std::min(tokens + (next - now) * fill, limit);
            now = next;
            if (!in_drive.empty() && done == now) {
                completed++;
                in_disk += now - in_drive.front().first;
                in_drive.pop_front();
                waiting++;
            } else if (waiting && ready == now) {
                tokens -= cost;
                sent += cost;
                waiting--;
                free_at = std::max(free_at, now) + service;
                in_drive.emplace_back(now, free_at);
            } else {
                break;
            }
        }
        double mean_in_disk = completed ? in_disk / completed : 0.0;
        tio.measured(std::exchange(completed, 0), std::exchange(in_disk, 0), std::exchange(sent, 0), start + std::chrono::seconds(second),
                std::exchange(occupancy, 0));
        tio.regulate(start + std::chrono::seconds(second));
        result.push_back({tio.rate_correction(), mean_in_disk});
        maybe_yield().get();
    }
    return result;
}

SEASTAR_THREAD_TEST_CASE(test_disk_rate_correction) {
    // A load, the limits of c, and the second from which c must get within the
    // limits. After c gets within the limits, it must stay there. c changes in
    // steps of 0.005, and the floating point values of c are not exact. Thus
    // each limit is half a step beyond the allowed values: 0.8875 allows 0.89
    // and not 0.885. On the drive at 20%, c can go down to 0.18 for some time,
    // because the deep queue in the drive takes c below the rate of the drive.
    struct expected {
        drive_load load;
        double lower_limit;
        double upper_limit;
        int from = 0;
    };
    const double threshold = rate_correction_config().disk_latency_threshold.count();
    for (const auto& [load, lower_limit, upper_limit, from] : std::initializer_list<expected>{
            {{.performance = 0.9}, 0.8875, 0.9125},
            {{.performance = 1.0}, 0.9875, 1.0025},
            {{.performance = 1.1}, 0.9975, 1.0025},
            {{.performance = 0.2}, 0.1775, 0.2125},
            // The largest writes cost nearly the limit of the bucket. The drive
            // needs more time to work off a queue of them. Thus c goes further
            // below the rate of the drive before it gets back up to it.
            {{.performance = 0.9, .write_size = 0}, 0.8875, 0.9125, 200},
            // The drive delivers all its rates again
            {{.performance = 0.9, .performance_later = 1.0, .later = 600, .seconds = 1800}, 0.9875, 1.0025, 600},
            // A load too light to fill the drive does not lower the rates
            {{.performance = 0.5, .writes = 4}, 0.9975, 1.0025}}) {
        auto what = fmt::format("{} writes of {}KiB on a drive at {}{}", load.writes, load.write_size >> 10, load.performance,
                load.later ? fmt::format(", then at {}", load.performance_later) : "");
        auto result = simulate_rate_correction(load);
        auto within_limits = [&] (const corrected_second& s) { return lower_limit < s.correction && s.correction < upper_limit; };
        auto first = std::find_if(result.begin() + from, result.end(), within_limits);
        BOOST_REQUIRE_MESSAGE(first != result.end(), what << ": correction never within limits");
        for (auto it = first; it != result.end(); ++it) {
            BOOST_REQUIRE_MESSAGE(within_limits(*it), what << ": correction " << it->correction
                    << " after " << it - result.begin() + 1 << "s, within limits since " << first - result.begin() + 1 << "s");
        }
        // After the drive works off its queue, the writes spend more than the
        // threshold in the drive in at most 10% of the seconds
        auto drained = std::find_if(first, result.end(), [&] (const corrected_second& s) { return s.in_disk <= threshold; });
        BOOST_REQUIRE_MESSAGE(drained != result.end(), what << ": the queue in the drive is never worked off");
        auto queued = std::count_if(drained, result.end(), [&] (const corrected_second& s) { return s.in_disk > threshold; });
        BOOST_REQUIRE_MESSAGE(queued * 10 <= result.end() - drained, what << ": the writes spend more than the threshold in the drive in "
                << queued << " of the last " << result.end() - drained << " seconds");
    }
}

SEASTAR_THREAD_TEST_CASE(test_disk_rate_correction_leaves_reactor_delay_out) {
    using namespace std::chrono_literals;
    auto dnl = internal::io_direction_and_length(internal::io_direction_and_length::write_idx, 4096);
    // For each write: its time in the drive, whether the reactor polls or
    // sleeps meanwhile, whether the write fails, and the least time in the
    // disk that the queue must count. After its time in the drive, each write
    // waits 30ms while the reactor is blocked. This time is not time in the
    // disk.
    struct write_case {
        std::chrono::milliseconds in_drive;
        bool reactor_sleeps;
        bool fails;
        std::chrono::milliseconds at_least;
    };
    for (auto [in_drive, reactor_sleeps, fails, at_least] : std::initializer_list<write_case>{
            {0ms, false, false, 0ms},
            {20ms, false, false, 15ms},
            {20ms, true, false, 15ms},
            {20ms, false, true, 0ms}}) {
        io_queue_for_tests tio(rate_correction_config());
        if (reactor_sleeps) {
            tio.kicker.cancel();
        }
        io_completion* write = nullptr;
        auto f = tio.queue_request(get_default_pc(), dnl, internal::io_request::make_write(0, 0, nullptr, 4096, false), nullptr, {});
        seastar::sleep(10ms).get();
        const auto before_dispatch = io_queue::clock_type::now();
        tio.queue.poll_io_queue();
        BOOST_REQUIRE_EQUAL(tio.sent(), tio.queue.request_capacity(dnl));
        tio.sink.drain([&write] (const internal::io_request&, io_completion* desc) { write = desc; return true; });
        if (in_drive.count()) {
            seastar::sleep(in_drive).get();
            // The previous poll can be before the dispatch, if the reactor
            // thread did not run during the sleep. After one more poll, the
            // previous poll is the one that ended the sleep.
            for (auto polls = engine().polls(); engine().polls() == polls; ) {
                seastar::yield().get();
            }
        }
        // The time in the disk ends at the previous poll. The reactor does
        // not poll from here until the write completes. Thus the time in the
        // disk ends before the block of the reactor.
        const auto at_most = std::max(engine().prev_poll_time() - before_dispatch, io_queue::clock_type::duration(0));
        std::this_thread::sleep_for(30ms);
        if (fails) {
            write->set_exception(std::make_exception_ptr(std::runtime_error("write failed")));
            BOOST_REQUIRE_THROW(f.get(), std::runtime_error);
        } else {
            write->complete_with(4096);
            f.get();
        }
        BOOST_REQUIRE_EQUAL(tio.completed_in_disk(), fails ? 0 : 1);
        BOOST_REQUIRE_GE(tio.time_in_disk_us(), std::chrono::microseconds(at_least).count());
        BOOST_REQUIRE_LE(tio.time_in_disk_us(), fails ? 0 : std::chrono::duration_cast<std::chrono::microseconds>(at_most).count());
        // The number of requests in the disk counts the same time, also for a
        // failed write
        BOOST_REQUIRE_GE(tio.occupancy_us(), std::chrono::microseconds(in_drive * 3 / 4).count());
        BOOST_REQUIRE_LE(tio.occupancy_us(), std::chrono::duration_cast<std::chrono::microseconds>(at_most).count());
        if (!fails) {
            BOOST_REQUIRE_EQUAL(tio.occupancy_us(), tio.time_in_disk_us());
        }
    }
}

SEASTAR_THREAD_TEST_CASE(test_disk_rate_correction_steps) {
    auto cfg = rate_correction_config();
    const double threshold = cfg.disk_latency_threshold.count();
    io_queue_for_tests tio(cfg);
    auto now = next_period_start();
    // Runs `seconds` probes. In each probe, `completed` writes spent `in_disk`
    // each in the drive, and the queue sent `use` of what its bucket let
    // through. Returns the change of c, in steps.
    auto probe = [&] (int seconds, double in_disk, double use = 1.0, uint64_t completed = 1000) {
        auto before = tio.rate_correction();
        while (seconds--) {
            now += std::chrono::seconds(1);
            tio.measured(completed, completed * in_disk, use * tio.rate_correction(), now);
            tio.regulate(now);
        }
        return std::lround((tio.rate_correction() - before) / cfg.rate_correction_step);
    };
    // A cycle is 30 probes. The first cycle starts with the first probe.
    BOOST_REQUIRE_EQUAL(probe(30, 2.1 * threshold), -30);         // Overloaded
    BOOST_REQUIRE_EQUAL(probe(1, 1.9 * threshold), 0);            // Draining
    BOOST_REQUIRE_EQUAL(probe(1, 1.9 * threshold), -1);           // Overloading
    BOOST_REQUIRE_EQUAL(probe(28, 0.9 * threshold), 0);           // Balanced
    BOOST_REQUIRE_EQUAL(probe(30, 0, 1.0, 0), 0);                 // no request completed
    BOOST_REQUIRE_EQUAL(probe(30, 0.55 * threshold), 0);          // Balanced
    BOOST_REQUIRE_EQUAL(probe(30, 0.4 * threshold, 0.96), 0);     // Not loaded
    probe(4, 0.9 * threshold);
    BOOST_REQUIRE_EQUAL(probe(26, 0.4 * threshold), 0);           // Spare capacity in 26 of 30 probes
    probe(3, 0.9 * threshold);
    BOOST_REQUIRE_EQUAL(probe(27, 0.4 * threshold), 1);           // Spare capacity in 27 of 30 probes
    probe(27, 0.4 * threshold);
    probe(2, 0.9 * threshold);
    BOOST_REQUIRE_EQUAL(probe(1, 2.1 * threshold), -1);           // an Overloaded probe ends the cycle
    probe(27, 0.4 * threshold);
    probe(2, 0.9 * threshold);
    BOOST_REQUIRE_EQUAL(probe(1, 0, 1.0, 0), 0);                  // a probe without completions ends the cycle
    probe(27, 0.4 * threshold);
    probe(1, 2.1 * threshold);
    probe(1, 1.95 * threshold);
    BOOST_REQUIRE_EQUAL(probe(1, 1.9 * threshold), 0);            // a Draining probe ends the cycle
    BOOST_REQUIRE_EQUAL(probe(30, 3 * threshold, 1.0, 1), 0);     // one slow write a second is not queueing
    probe(210, 2.1 * threshold);
    BOOST_REQUIRE_CLOSE(tio.rate_correction(), cfg.rate_correction_min, 1e-6);
    probe(27, 0.4 * threshold);
    probe(2, 0.9 * threshold);
    BOOST_REQUIRE_EQUAL(probe(1, 2.1 * threshold), 0);            // at the minimum, an Overloaded probe ends the cycle
    // The periods that the regulator skips do not count in the cycle
    probe(27, 0.4 * threshold);
    now += std::chrono::seconds(60);
    BOOST_REQUIRE_EQUAL(probe(2, 0.9 * threshold), 0);            // 29 probes
    BOOST_REQUIRE_EQUAL(probe(1, 0.9 * threshold), 1);            // room in 27 of 30 probes
}

SEASTAR_THREAD_TEST_CASE(test_disk_rate_correction_needs_every_group) {
    using namespace std::chrono_literals;
    // A group of a duplex disk has a bucket for reads and a bucket for writes.
    // The groups send only writes.
    for (bool duplex : {false, true}) {
        auto cfg = rate_correction_config();
        cfg.duplex = duplex;
        io_queue_for_tests first(cfg), second(cfg, first.group.get());
        auto now = next_period_start();
        // A second in which 1000 writes spent `in_disk` each in the drive. The
        // first group, or both groups, sent all that their buckets let
        // through. The first queue runs the regulator. Thus the second queue
        // writes its report first.
        auto measure = [&] (double in_disk, bool both_send) {
            now += 1s;
            second.measured(0, 0, both_send ? first.rate_correction() : 0.0, now);
            first.measured(1000, 1000 * in_disk, first.rate_correction(), now);
            first.regulate(now);
        };
        for (int i = 0; i < 20; i++) {
            measure(0.02, true);
        }
        auto corrected = first.rate_correction();
        BOOST_REQUIRE_LT(corrected, 1.0);
        for (int i = 0; i < 120; i++) {
            measure(0.0005, false);
        }
        BOOST_REQUIRE_EQUAL(first.rate_correction(), corrected);
        for (int i = 0; i < 120; i++) {
            measure(0.0005, true);
        }
        BOOST_REQUIRE_MESSAGE(first.rate_correction() > corrected, (duplex ? "duplex" : "simplex") << " disk: correction "
                << first.rate_correction() << ", not above " << corrected);
    }
}

SEASTAR_THREAD_TEST_CASE(test_disk_rate_correction_waits_for_every_queue) {
    using namespace std::chrono_literals;
    auto cfg = rate_correction_config();
    // The time from the end of a second until the regulator reads the reports
    const auto read = io_queue_for_tests(cfg).read_delay();
    // The time from the end of each second until the second queue writes its
    // report, and whether c must decrease. The regulator does not decide on a
    // second without all reports.
    for (auto [delay, decreases] : std::initializer_list<std::pair<std::optional<io_queue::clock_type::duration>, bool>>{
            {10ms, true}, {read - 1ms, true}, {read + 1ms, false}, {std::nullopt, false}}) {
        io_queue_for_tests first(cfg), second(cfg, first.group.get());
        auto now = next_period_start();
        for (int i = 0; i < 20; i++) {
            now += 1s;
            // 1000 writes of the first queue spent 20ms each in the drive,
            // four times the threshold. The first queue runs the regulator.
            first.measured(1000, 20, 1.0, now);
            if (delay && *delay < read) {
                second.measured(0, 0, 1.0, now + *delay);
            }
            first.regulate(now);
            if (delay && *delay >= read) {
                second.measured(0, 0, 1.0, now + *delay);
            }
        }
        BOOST_REQUIRE_MESSAGE((first.rate_correction() < 1.0) == decreases, "second queue reports "
                << (delay ? std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(*delay).count()) + "ms late" : "never")
                << ", the regulator reads " << std::chrono::duration_cast<std::chrono::milliseconds>(read).count()
                << "ms late: correction " << first.rate_correction());
    }
}

SEASTAR_THREAD_TEST_CASE(test_disk_rate_correction_does_not_wait_for_a_later_queue) {
    using namespace std::chrono_literals;
    auto cfg = rate_correction_config();
    io_queue_for_tests first(cfg);
    // The first queue measures a whole period. The second queue starts after
    // the end of that period. Thus it has no report for that period.
    auto now = next_period_start() + 1s;
    seastar::sleep(now - io_queue::clock_type::now() + 10ms).get();
    io_queue_for_tests second(cfg, first.group.get());
    // 1000 writes of the first queue spent 20ms each in the drive, four
    // times the threshold
    first.measured(1000, 20, 1.0, now);
    first.regulate(now);
    BOOST_REQUIRE_LT(first.rate_correction(), 1.0);
}

SEASTAR_THREAD_TEST_CASE(test_disk_rate_correction_measures_the_real_time) {
    using namespace std::chrono_literals;
    // A report covers the time from the previous report of the queue. For
    // each case: how late the queue writes its reports of two periods, the
    // number of writes in the second period, and whether c must decrease.
    // Each write spends 11ms in the drive, more than twice the threshold.
    // Thus c decreases if the disk holds at least 8 writes on average in the
    // time that the report covers. The regulator skips the period if a report
    // covers less than 0.5s.
    struct report_case {
        std::chrono::milliseconds first_late;
        std::chrono::milliseconds second_late;
        uint64_t writes;
        bool decreases;
    };
    for (auto [first_late, second_late, writes, decreases] : std::initializer_list<report_case>{
            {0ms, 70ms, 739, false},        // 1.07s with 7.6 writes in the disk
            {70ms, 0ms, 700, true},         // 0.93s with 8.3 writes in the disk
            {400ms, 0ms, 600, true},        // 0.6s with 11 writes in the disk
            {600ms, 0ms, 400, false}}) {    // 0.4s with 11 writes in the disk
        io_queue_for_tests tio(rate_correction_config());
        auto now = next_period_start() + 1s;
        tio.measured(0, 0, 0, now + first_late);
        now += 1s;
        tio.measured(writes, writes * 0.011, 1.0, now + second_late);
        tio.regulate(now);
        BOOST_REQUIRE_MESSAGE((tio.rate_correction() < 1.0) == decreases, writes << " writes, reports " << first_late.count()
                << "ms and " << second_late.count() << "ms late: correction " << tio.rate_correction());
    }
    io_queue_for_tests tio(rate_correction_config());
    auto now = next_period_start();
    // 1000 writes spent 20ms each in the drive, four times the threshold
    for (int i = 0; i < 20; i++) {
        now += 1s;
        tio.measured(1000, 20, 1.0, now);
        tio.regulate(now);
    }
    auto corrected = tio.rate_correction();
    BOOST_REQUIRE_LT(corrected, 1.0);
    // The queue writes every second report 70ms late, as at a tick. Thus its
    // reports are for 1.07s and 0.93s in turn. In that time, the queue sends
    // all that its bucket lets through at c, and the writes spend 0.5ms each
    // in the drive.
    for (int i = 0; i < 60; i++) {
        now += 1s;
        tio.measured(1000, 0.5, 1.07 * tio.rate_correction(), now + 70ms);
        tio.regulate(now);
        now += 1s;
        tio.measured(1000, 0.5, 0.93 * tio.rate_correction(), now);
        tio.regulate(now);
    }
    BOOST_REQUIRE_GT(tio.rate_correction(), corrected);
}

SEASTAR_THREAD_TEST_CASE(test_disk_rate_correction_skips_the_periods_it_missed) {
    using namespace std::chrono_literals;
    auto cfg = rate_correction_config();
    io_queue_for_tests tio(cfg);
    auto now = next_period_start();
    // 1000 writes spent 20ms each in the drive, four times the threshold.
    // Thus each probe decreases c.
    now += 1s;
    tio.measured(1000, 20, 1.0, now);
    tio.regulate(now);
    auto once = tio.rate_correction();
    BOOST_REQUIRE_LT(once, 1.0);
    // A stall delays the regulator past the end of the next period. It skips
    // that period and probes only the newest one.
    now += 1s;
    tio.measured(1000, 20, 1.0, now);
    now += 1s;
    tio.measured(1000, 20, 1.0, now);
    tio.regulate(now);
    BOOST_REQUIRE_CLOSE(tio.rate_correction(), once - cfg.rate_correction_step, 1e-6);
    // After each run, the timer of the regulator fires at the next time that
    // is the read delay after the end of a period
    const auto delay = tio.read_delay();
    BOOST_REQUIRE(tio.next_regulation(now + 1ms) == now + delay);
    BOOST_REQUIRE(tio.next_regulation(now + delay + 1ms) == now + 1s + delay);
    BOOST_REQUIRE(tio.next_regulation(now + 2s + delay + 1ms) == now + 3s + delay);
}

SEASTAR_THREAD_TEST_CASE(test_disk_rate_correction_needs_a_short_latency_goal) {
    using namespace std::chrono_literals;
    // A queue that does not poll writes its report at its tick, every 100
    // times the latency goal. The regulator reads the reports 50ms after the
    // tick, and at most 0.9s after the end of a period. For each case: the
    // latency goal and the threshold, in milliseconds, and whether Seastar
    // accepts them. If Seastar accepts them, the queue of a disk with
    // configured rates gets the threshold. A disk without configured rates
    // has nothing to correct.
    for (auto [goal, threshold, accepted] : std::initializer_list<std::tuple<double, double, bool>>{
            {8.5, 3, true}, {8.6, 3, false}, {20, 0, true}}) {
        smp_options smp_opts(nullptr);
        reactor_options reactor_opts(nullptr);
        reactor_opts.io_latency_goal_ms.set_value(goal);
        reactor_opts.io_disk_rate_reduction_latency_threshold_ms.set_value(threshold);
        internal::disk_config_params params(1);
        if (accepted) {
            BOOST_REQUIRE_NO_THROW(params.parse_config(smp_opts, reactor_opts));
            internal::disk_params disk;
            BOOST_REQUIRE_EQUAL(params.generate_config(disk, 0, 1).disk_latency_threshold.count(), 0);
            disk.read_bytes_rate = disk.write_bytes_rate = 1 << 30;
            disk.read_req_rate = disk.write_req_rate = 100000;
            auto configured = params.generate_config(disk, 0, 1).disk_latency_threshold;
            BOOST_REQUIRE_EQUAL(std::chrono::round<std::chrono::microseconds>(configured).count(), threshold * 1000);
        } else {
            BOOST_REQUIRE_THROW(params.parse_config(smp_opts, reactor_opts), std::runtime_error);
        }
    }
    // For a disk with very low rates, the latency goal of a group is longer
    // than the option. Then the rate correction is off for the disk.
    io_queue_for_tests tio(rate_correction_config(4 << 20));
    BOOST_REQUIRE_GT(tio.group->io_latency_goal(), 8.5ms);
    auto now = next_period_start();
    for (int i = 0; i < 5; i++) {
        now += 1s;
        tio.measured(1000, 20, 1.0, now);
        tio.regulate(now);
    }
    BOOST_REQUIRE_EQUAL(tio.rate_correction(), 1.0);
}

SEASTAR_THREAD_TEST_CASE(test_disk_rate_correction_with_its_timers) {
    using namespace std::chrono_literals;
    // Periods of 100ms and ticks of 7ms. Thus the timers of the queue run the
    // regulator many times in each case. For each case: the number of writes
    // that the load keeps in the queue and in the drive, the time for which
    // the drive holds each write, the time of the load, and whether c must
    // decrease. The drive holds each write for at least five times the
    // threshold. One write that the drive holds for 15 periods is one request
    // in the disk, not a queue.
    struct load_case {
        unsigned writes_in_flight;
        io_queue::clock_type::duration hold;
        io_queue::clock_type::duration load;
        bool decreases;
    };
    for (auto [writes_in_flight, hold, load, decreases] : std::initializer_list<load_case>{
            {32, 10ms, 2s, true},
            {1, 1500ms, 1s, false}}) {
        auto cfg = rate_correction_config();
        cfg.disk_latency_threshold = 2ms;
        cfg.rate_correction_period = 100ms;
        cfg.rate_correction_margin = 5ms;
        cfg.averaging_decay_ticks = 10;
        io_queue_for_tests tio(cfg, nullptr, true);
        auto dnl = internal::io_direction_and_length(internal::io_direction_and_length::write_idx, 4096);
        std::deque<std::pair<io_queue::clock_type::time_point, io_completion*>> in_drive;
        std::vector<future<size_t>> writes;
        unsigned done = 0;
        auto drive = [&] (io_queue::clock_type::duration held) {
            tio.queue.poll_io_queue();
            tio.sink.drain([&] (const internal::io_request&, io_completion* desc) {
                in_drive.emplace_back(io_queue::clock_type::now() + held, desc);
                return true;
            });
            while (!in_drive.empty() && in_drive.front().first <= io_queue::clock_type::now()) {
                in_drive.front().second->complete_with(4096);
                in_drive.pop_front();
                done++;
            }
        };
        for (auto end = io_queue::clock_type::now() + load; io_queue::clock_type::now() < end; ) {
            while (writes.size() - done < writes_in_flight) {
                writes.push_back(tio.queue_request(get_default_pc(), dnl, internal::io_request::make_write(0, 0, nullptr, 4096, false), nullptr, {}));
            }
            drive(hold);
            seastar::sleep(1ms).get();
        }
        // A write that fails before it enters the queue never reaches the
        // drive. Thus wait for the result of each write, then report the
        // first error.
        while (std::any_of(writes.begin(), writes.end(), [] (const future<size_t>& f) { return !f.available(); })) {
            drive(0ms);
            seastar::yield().get();
        }
        for (auto& f : writes) {
            f.get();
        }
        // The regulator probes the period of the last completion after the
        // end of that period
        seastar::sleep(std::chrono::duration_cast<io_queue::clock_type::duration>(2 * cfg.rate_correction_period) + tio.read_delay()).get();
        BOOST_REQUIRE_MESSAGE((tio.rate_correction() < 1.0) == decreases, writes_in_flight << " writes, each held "
                << std::chrono::duration_cast<std::chrono::milliseconds>(hold).count() << "ms: correction " << tio.rate_correction());
        BOOST_REQUIRE_EQUAL(internal::get_throttler(tio.queue, 0).token_bucket().rate() < io_throttler::fixed_point_factor, decreases);
    }
}

SEASTAR_THREAD_TEST_CASE(test_get_all_io_queues) {
    auto queues = engine().get_all_io_queues();
    // The test environment should have at least one io_queue configured
    BOOST_CHECK(!queues.empty());
    // Verify no duplicates (io_queues can be shared between devices)
    std::sort(queues.begin(), queues.end());
    auto it = std::unique(queues.begin(), queues.end());
    BOOST_CHECK(it == queues.end());
    // The default io_queue (device 0) should be in the returned set
    auto& default_queue = engine().get_io_queue(0);
    BOOST_CHECK(std::find(queues.begin(), queues.end(), &default_queue) != queues.end());
    // Every queue reachable via try_get_io_queue for known devices
    // should appear in the result. Stat the current directory to
    // discover its device and verify the queue for that device is present.
    auto sd = file_stat(".").get();
    if (auto* q = engine().try_get_io_queue(sd.device_id)) {
        BOOST_CHECK(std::find(queues.begin(), queues.end(), q) != queues.end());
    }
}
