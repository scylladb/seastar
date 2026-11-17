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

#include <seastar/core/shared_ptr.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/make_task.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/sstring.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/random.hh>
#include <seastar/util/later.hh>
#include <seastar/util/memory-data-source.hh>
#include <vector>
#include <list>
#include <deque>
#include <limits>
#include <optional>
#include <sstream>
#include "memory-data-sink.hh"

using namespace seastar;
using namespace net;

struct stream_maker {
    output_stream_options opts;
    size_t _size;

    stream_maker size(size_t size) && {
        _size = size;
        return std::move(*this);
    }

    stream_maker trim(bool do_trim) && {
        opts.trim_to_size = do_trim;
        return std::move(*this);
    }

    lw_shared_ptr<output_stream<char>> operator()(data_sink sink) {
        return make_lw_shared<output_stream<char>>(std::move(sink), _size, opts);
    }
};

// A sink that records received chunk sizes and concatenates all data,
// allowing invariant-based assertions rather than exact chunk matching.
class invariant_checker_sink final : public data_sink_impl {
    std::string& _received_data;
    std::vector<size_t>& _chunk_sizes;
public:
    invariant_checker_sink(std::string& received_data, std::vector<size_t>& chunk_sizes)
        : _received_data(received_data)
        , _chunk_sizes(chunk_sizes)
    { }

    future<> put(std::span<temporary_buffer<char>> bufs) override {
        size_t put_size = 0;
        for (auto&& buf : bufs) {
            _received_data.append(buf.get(), buf.size());
            put_size += buf.size();
        }
        if (put_size > 0) {
            _chunk_sizes.push_back(put_size);
        }
        return make_ready_future<>();
    }

    future<> close() override { return make_ready_future<>(); }
};

// Iterates over all combinations of 1..MAX_CHUNKS chunks, each 1..MAX_CHUNK_SIZE
// bytes, for both trim modes, and both buffered and zero-copy write paths.
//
// STREAM_SIZE=5 is chosen as a small non-power-of-two value.
// MAX_CHUNK_SIZE=3*STREAM_SIZE ensures the split loop is exercised for at
// least three full buffer-lengths in a single write.
// MAX_CHUNKS=4 keeps the combination space tractable (~11k sequences).
static constexpr size_t STREAM_SIZE = 5;
static constexpr size_t MAX_CHUNKS = 4;
static constexpr size_t MAX_CHUNK_SIZE = 3 * STREAM_SIZE;

enum class write_type { buffered, zero_copy };

static std::string format_context(const std::vector<size_t>& input_chunk_sizes,
        size_t stream_size, bool trim_to_size,
        const std::vector<write_type>& write_types) {
    std::ostringstream os;
    os << "stream_size=" << stream_size
       << " trim_to_size=" << trim_to_size
       << " input_chunks=[";
    for (size_t i = 0; i < input_chunk_sizes.size(); i++) {
        if (i > 0) os << ", ";
        os << input_chunk_sizes[i];
    }
    os << "] write_types=[";
    for (size_t i = 0; i < write_types.size(); i++) {
        if (i > 0) os << ", ";
        os << (write_types[i] == write_type::buffered ? "buffered" : "zero_copy");
    }
    os << "]";
    return os.str();
}

// Checks the output invariants after all writes and close():
// - data integrity: concatenation of output == concatenation of input
// - no empty chunks reach the sink
// - for trim_to_size=true:  all non-last chunks are exactly _size bytes
// - for trim_to_size=false: all non-last chunks are >= _size bytes
// - if nothing was written, the sink receives no chunks at all
static void check_invariants(const std::string& expected_data,
        const std::vector<size_t>& chunk_sizes,
        const std::string& received_data,
        size_t stream_size, bool trim_to_size,
        const std::string& ctx) {
    BOOST_REQUIRE_MESSAGE(received_data == expected_data,
            "data integrity check failed: " << ctx);

    if (expected_data.empty()) {
        BOOST_REQUIRE_MESSAGE(chunk_sizes.empty(),
                "no chunks expected for empty write: " << ctx);
        return;
    }

    BOOST_REQUIRE_MESSAGE(chunk_sizes.back() > 0,
            "sink must never receive an empty chunk: " << ctx);

    for (size_t i = 0; i + 1 < chunk_sizes.size(); i++) {
        BOOST_REQUIRE_MESSAGE(chunk_sizes[i] > 0,
                "sink must never receive an empty chunk: " << ctx);
        if (trim_to_size) {
            BOOST_REQUIRE_MESSAGE(chunk_sizes[i] == stream_size,
                    "with trim_to_size all non-last chunks must be exactly _size bytes: " << ctx);
        } else {
            BOOST_REQUIRE_MESSAGE(chunk_sizes[i] >= stream_size,
                    "without trim_to_size all non-last chunks must be >= _size bytes: " << ctx);
        }
    }
}

// Calls fn(chunk_sizes) for every combination of 1..MAX_CHUNKS chunks
// each of size 1..MAX_CHUNK_SIZE.
template <typename Fn>
static void for_each_chunk_combination(Fn fn) {
    std::vector<size_t> combo;
    std::function<void()> recurse = [&]() {
        if (!combo.empty()) {
            fn(combo);
        }
        if (combo.size() < MAX_CHUNKS) {
            for (size_t sz = 1; sz <= MAX_CHUNK_SIZE; sz++) {
                combo.push_back(sz);
                recurse();
                combo.pop_back();
            }
        }
    };
    recurse();
}

// Calls fn(write_types) for every assignment of buffered/zero_copy to
// n write calls (2^n patterns total; n <= MAX_CHUNKS so at most 16).
#ifndef SEASTAR_DEBUG
template <typename Fn>
static void for_each_type_pattern(size_t n, Fn fn) {
    std::vector<write_type> pattern(n);
    for (size_t mask = 0; mask < (size_t(1) << n); mask++) {
        for (size_t i = 0; i < n; i++) {
            pattern[i] = (mask >> i) & 1 ? write_type::zero_copy : write_type::buffered;
        }
        fn(pattern);
    }
}
#else
// In SEASTAR_DEBUG builds, sample ~10% of patterns to keep sanitizer
// run times acceptable. The all-buffered (0...0) and all-zero-copy
// (1...1) patterns are always included.
template <typename Fn>
static void for_each_type_pattern(size_t n, Fn fn) {
    const size_t total = size_t(1) << n;
    size_t sample_size = std::max(size_t(2), size_t(std::round(total * 0.1)));

    std::vector<size_t> masks(total);
    std::iota(masks.begin(), masks.end(), 0);
    std::shuffle(masks.begin(), masks.end(), seastar::testing::local_random_engine);
    // Ensure all-buffered (0) and all-zero-copy (total-1) are always first.
    std::swap(*std::find(masks.begin(), masks.end(), size_t(0)), masks[0]);
    std::swap(*std::find(masks.begin(), masks.end(), total - 1), masks[1]);

    std::vector<write_type> pattern(n);
    for (size_t i = 0; i < sample_size; i++) {
        for (size_t j = 0; j < n; j++) {
            pattern[j] = (masks[i] >> j) & 1 ? write_type::zero_copy : write_type::buffered;
        }
        fn(pattern);
    }
}
#endif

// Builds a string of `len` bytes filled with a cycling pattern,
// so that data integrity failures produce readable diffs.
static std::string make_data(size_t len) {
    std::string s(len, '\0');
    for (size_t i = 0; i < len; i++) {
        s[i] = 'a' + (i % 26);
    }
    return s;
}

SEASTAR_THREAD_TEST_CASE(test_splitting_invariants) {
    for (bool trim_to_size : {false, true}) {
        for_each_chunk_combination([&](const std::vector<size_t>& chunk_sizes) {
            for_each_type_pattern(chunk_sizes.size(), [&](const std::vector<write_type>& write_types) {
                std::string received_data;
                std::vector<size_t> out_chunk_sizes;
                auto mk = stream_maker().trim(trim_to_size).size(STREAM_SIZE);
                auto out = mk(data_sink(std::make_unique<invariant_checker_sink>(
                        received_data, out_chunk_sizes)));

                std::string expected_data;
                for (size_t i = 0; i < chunk_sizes.size(); i++) {
                    auto data = make_data(chunk_sizes[i]);
                    expected_data += data;
                    switch (write_types[i]) {
                    case write_type::buffered:
                        out->write(data).get();
                        break;
                    case write_type::zero_copy:
                        out->write(temporary_buffer<char>::copy_of(data)).get();
                        break;
                    }
                }
                out->close().get();
                check_invariants(expected_data, out_chunk_sizes, received_data,
                        STREAM_SIZE, trim_to_size,
                        format_context(chunk_sizes, STREAM_SIZE, trim_to_size, write_types));
            });
        });
    }
}

SEASTAR_THREAD_TEST_CASE(test_flush_on_empty_buffer_does_not_push_empty_packet_down_stream) {
    std::stringstream ss;
    auto out = output_stream<char>(testing::memory_data_sink(ss), 8);

    out.flush().get();
    BOOST_REQUIRE(ss.str().empty());
}

SEASTAR_THREAD_TEST_CASE(test_move_after_batched_write) {
    std::stringstream ss;
    std::optional<output_stream<char>> out;
    {
        auto out2 = output_stream<char>(testing::memory_data_sink(ss), 8);
        out2.write(sstring("smth")).get();
        out.emplace(std::move(out2));
    }
    out->close().get();
    BOOST_REQUIRE_EQUAL(ss.str(), "smth");
}

namespace {

// Build an input_stream that serves `data` as fixed-size chunks, using the
// in-tree util::memory_data_source. Multiple chunks exercise copy_n's read
// loop even for small inputs.
input_stream<char> make_chunked_input_stream(std::string_view data, size_t chunk = 4) {
    std::vector<temporary_buffer<char>> bufs;
    for (size_t i = 0; i < data.size(); i += chunk) {
        auto sz = std::min(chunk, data.size() - i);
        bufs.emplace_back(data.data() + i, sz);
    }
    return util::as_input_stream(std::move(bufs));
}

}

SEASTAR_THREAD_TEST_CASE(test_copy_n) {
    const sstring src = "0123456789abcdef"; // 16 bytes; 4-byte chunks => multi-read

    // copy_n copies exactly n bytes; check counts spanning the chunk boundary,
    // and that the input retains exactly the unconsumed remainder afterwards.
    for (size_t n : {size_t(0), size_t(1), size_t(4), size_t(5), size_t(13), src.size()}) {
        std::stringstream ss;
        auto in = make_chunked_input_stream(src);
        auto out = output_stream<char>(testing::memory_data_sink(ss), 8);
        copy_n(in, out, n).get();
        out.close().get();
        BOOST_REQUIRE_EQUAL(ss.str(), std::string(src.data(), n));

        // The input must still have exactly the remaining bytes available.
        auto rest = in.read_exactly(src.size() - n).get();
        BOOST_REQUIRE_EQUAL(sstring(rest.get(), rest.size()), src.substr(n));
    }

    // Requesting more than the stream holds fails (exactly-n contract).
    {
        std::stringstream ss;
        auto in = make_chunked_input_stream(src);
        auto out = output_stream<char>(testing::memory_data_sink(ss), 8);
        BOOST_REQUIRE_THROW(copy_n(in, out, src.size() + 1).get(), std::runtime_error);
        out.close().get();
    }
}

SEASTAR_THREAD_TEST_CASE(test_simple_write) {
    std::stringstream ss;
    auto out = output_stream<char>(testing::memory_data_sink(ss), 8);

    auto value1 = sstring("te");
    out.write(value1).get();


    auto value2 = sstring("st");
    out.write(value2).get();

    auto value3 = sstring("abcdefgh1234");
    out.write(value3).get();

    out.close().get();

    auto value = value1 + value2 + value3;

    BOOST_REQUIRE_EQUAL(ss.str(), value);
}

namespace seastar::testing {

class output_stream_test {
public:
    static bool has_buffer(const ::output_stream<char>& out) {
        return out._end;
    }
    static bool has_zc(const ::output_stream<char>& out) {
        return !out._zc_bufs.empty();
    }
    static bool in_batch(const ::output_stream<char>& out) {
        return out._in_batch.has_value();
    }
};

}

SEASTAR_THREAD_TEST_CASE(test_mixed_mode_write) {
    std::stringstream ss;
    auto out = output_stream<char>(testing::memory_data_sink(ss), 8);

    // First -- put some data in "buffered" mode and check that
    // stream gains a buffer but not a zc packet
    out.write("te", 2).get();
    BOOST_REQUIRE(testing::output_stream_test::has_buffer(out) && !testing::output_stream_test::has_zc(out));
    // Second -- append some zero-copy buffer and check that the
    // buffer disappears in favor of a bunch of zc packets (implementation detail, but still)
    out.write(temporary_buffer<char>("st", 2)).get();
    BOOST_REQUIRE(!testing::output_stream_test::has_buffer(out) && testing::output_stream_test::has_zc(out));

    // Finally -- all data must go away after flush
    out.flush().get();
    BOOST_REQUIRE(!testing::output_stream_test::has_buffer(out) && !testing::output_stream_test::has_zc(out));

    out.close().get();

    BOOST_REQUIRE_EQUAL(ss.str(), "test");
}

namespace {

// What a batching_sink was given, and how its next put() and flush() complete.
struct batching_sink_state {
    std::string data;
    unsigned puts = 0;
    unsigned flushes = 0;
    unsigned flush_errors = 0;
    // put() fails at once.
    bool fail_put = false;
    // put() and flush() return the future of pending_put and pending_flush,
    // for the test to resolve with complete_put() and complete_flush().
    bool hold_put = false;
    bool hold_flush = false;
    std::optional<promise<>> pending_put;
    std::optional<promise<>> pending_flush;

    /// Resolves the held put(), failing it with \c ex if that is set.
    void complete_put(std::exception_ptr ex = nullptr) {
        complete(pending_put, std::move(ex));
    }
    /// Resolves the held flush(), failing it with \c ex if that is set.
    void complete_flush(std::exception_ptr ex = nullptr) {
        complete(pending_flush, std::move(ex));
    }
private:
    static void complete(std::optional<promise<>>& pending, std::exception_ptr ex) {
        BOOST_CHECK(pending);
        if (auto pr = std::exchange(pending, std::nullopt)) {
            if (ex) {
                pr->set_exception(std::move(ex));
            } else {
                pr->set_value();
            }
        }
    }
};

// A sink that supports batched flushes and lets the test control the outcome
// of each put() and flush().
class batching_sink final : public data_sink_impl {
    batching_sink_state& _s;
public:
    explicit batching_sink(batching_sink_state& s) noexcept : _s(s) {}

    future<> put(std::span<temporary_buffer<char>> bufs) override {
        ++_s.puts;
        if (_s.fail_put) {
            return make_exception_future<>(std::runtime_error("put failed"));
        }
        for (auto& buf : bufs) {
            _s.data.append(buf.get(), buf.size());
        }
        if (_s.hold_put) {
            return _s.pending_put.emplace().get_future();
        }
        return make_ready_future<>();
    }
    future<> flush() override {
        ++_s.flushes;
        if (_s.hold_flush) {
            return _s.pending_flush.emplace().get_future();
        }
        return make_ready_future<>();
    }
    future<> close() override { return make_ready_future<>(); }
    size_t buffer_size() const noexcept override { return 8; }
    bool can_batch_flushes() const noexcept override { return true; }
    void on_batch_flush_error() noexcept override { ++_s.flush_errors; }
};

// A stream that batches flushes into a batching_sink. While a batch is in
// flight the tests check with BOOST_CHECK rather than BOOST_REQUIRE, and
// close() completes whatever is still held before it closes the stream, so
// that a failed check is reported instead of leaving a batch in flight for
// the assertion in ~output_stream to abort on.
struct batching_stream {
    batching_sink_state s;
    output_stream<char> out;
    bool closed = false;

    batching_stream() : out(make_out(s)) {}
    ~batching_stream() {
        if (!closed) {
            try {
                close();
            } catch (...) {
                BOOST_ERROR("close() failed");
            }
        }
    }
    /// Completes any held put() and flush(), stops holding new ones, and
    /// closes the stream.
    void close() {
        closed = true;
        s.hold_put = false;
        s.hold_flush = false;
        s.fail_put = false;
        if (s.pending_put) {
            s.complete_put();
        }
        if (s.pending_flush) {
            s.complete_flush();
        }
        out.close().get();
    }
private:
    static output_stream<char> make_out(batching_sink_state& s) {
        output_stream_options opts;
        opts.batch_flushes = true;
        return output_stream<char>(data_sink(std::make_unique<batching_sink>(s)), 8, opts);
    }
};

// A batched flush runs from a reactor poller, which yield() alone does not
// give a chance to run, so sleep until pred() holds, for at most 10 seconds.
// Returns whether it holds.
template <typename Pred>
bool wait_for(Pred pred) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
        seastar::sleep(std::chrono::milliseconds(1)).get();
    }
    return pred();
}

bool in_batch(const output_stream<char>& out) {
    return testing::output_stream_test::in_batch(out);
}

}

// A batched flush whose put has to wait calls the sink flush once the put
// completes, and finishes the batch in the same task.
SEASTAR_THREAD_TEST_CASE(test_batched_flush_put_waits) {
    batching_stream b;
    auto& s = b.s;
    // A marker task is queued before and after the completion of the put,
    // and the tasks that run between them are counted. Other work of the
    // reactor, such as a timer or a cross-shard message, can run between the
    // markers and add to the count, but never take from it, so keep the
    // smallest count of a few rounds.
    uint64_t min_tasks = std::numeric_limits<uint64_t>::max();
    for (unsigned round = 1; round <= 5; ++round) {
        s.hold_put = true;
        b.out.write("abc").get();
        b.out.flush().get();
        BOOST_CHECK(wait_for([&] { return s.puts == round; }));
        BOOST_CHECK_EQUAL(s.flushes, round - 1);
        BOOST_CHECK(in_batch(b.out));

        s.hold_put = false;
        // Shared with the markers, so that it outlives them even if the round
        // ends before they run.
        struct marker_counts {
            std::optional<uint64_t> before;
            std::optional<uint64_t> after;
        };
        auto counts = make_lw_shared<marker_counts>();
        seastar::schedule(make_task([counts] {
            counts->before = engine().get_sched_stats().tasks_processed;
        }));
        s.complete_put();
        seastar::schedule(make_task([counts] {
            counts->after = engine().get_sched_stats().tasks_processed;
        }));
        BOOST_CHECK(wait_for([&] { return !in_batch(b.out) && counts->before && counts->after; }));
        BOOST_CHECK_EQUAL(s.flushes, round);
        if (counts->before && counts->after) {
            // tasks_processed counts a task once it ends, so the first marker
            // is counted in after as well.
            min_tasks = std::min(min_tasks, *counts->after - *counts->before - 1);
        }
    }
#if !defined(SEASTAR_SHUFFLE_TASK_QUEUE) && !defined(SEASTAR_DEBUG)
    // With the task queue shuffled the markers do not bracket the completion,
    // and SEASTAR_DEBUG adds a task to every put.
    BOOST_CHECK_EQUAL(min_tasks, 1u);
#endif
    b.close();
    BOOST_CHECK_EQUAL(s.data, "abcabcabcabcabc");
    BOOST_CHECK_EQUAL(s.flush_errors, 0u);
}

// If the sink flush of a batched flush has to wait, the batch stays in
// progress, and a put issued meanwhile waits for it, until the sink flush
// completes.
SEASTAR_THREAD_TEST_CASE(test_batched_flush_sink_flush_waits) {
    batching_stream b;
    auto& s = b.s;
    s.hold_flush = true;
    b.out.write("abc").get();
    b.out.flush().get();
    BOOST_CHECK(wait_for([&] { return s.flushes == 1; }));
    BOOST_CHECK_EQUAL(s.puts, 1u);
    BOOST_CHECK(in_batch(b.out));

    auto write_f = b.out.write(sstring(8, 'x'));
    seastar::yield().get();
    BOOST_CHECK(!write_f.available());
    BOOST_CHECK_EQUAL(s.puts, 1u);

    s.hold_flush = false;
    s.complete_flush();
    write_f.get();
    BOOST_CHECK_EQUAL(s.puts, 2u);
    b.close();
    BOOST_CHECK_EQUAL(s.data, "abcxxxxxxxx");
    BOOST_CHECK_EQUAL(s.flush_errors, 0u);
}

// A failed sink flush that had to wait is reported like any other batched
// flush error.
SEASTAR_THREAD_TEST_CASE(test_batched_flush_sink_flush_fails) {
    batching_stream b;
    auto& s = b.s;
    s.hold_flush = true;
    b.out.write("abc").get();
    b.out.flush().get();
    BOOST_CHECK(wait_for([&] { return s.flushes == 1; }));

    s.hold_flush = false;
    s.complete_flush(std::make_exception_ptr(std::runtime_error("flush failed")));
    BOOST_CHECK(wait_for([&] { return !in_batch(b.out); }));
    BOOST_CHECK_EQUAL(s.flush_errors, 1u);
    BOOST_CHECK_THROW(b.out.flush().get(), std::runtime_error);
    b.close();
    BOOST_CHECK_EQUAL(s.flush_errors, 1u);
}

// If the put of a batched flush fails, the sink is not flushed, the sink is
// told about the error once, and the next flush() returns the error.
SEASTAR_THREAD_TEST_CASE(test_batched_flush_put_fails) {
    batching_stream b;
    auto& s = b.s;
    s.fail_put = true;
    b.out.write("abc").get();
    b.out.flush().get();
    BOOST_CHECK(wait_for([&] { return s.puts == 1 && !in_batch(b.out); }));
    BOOST_CHECK_EQUAL(s.flushes, 0u);
    BOOST_CHECK_EQUAL(s.flush_errors, 1u);

    s.fail_put = false;
    BOOST_CHECK_THROW(b.out.flush().get(), std::runtime_error);
    b.close();
    BOOST_CHECK_EQUAL(s.flush_errors, 1u);
}

// A put that waits for a batched flush whose own put then fails goes ahead
// once the batch ends; the error is delivered by the next flush().
SEASTAR_THREAD_TEST_CASE(test_batched_flush_put_fails_with_waiting_writer) {
    batching_stream b;
    auto& s = b.s;
    s.hold_put = true;
    b.out.write("abc").get();
    b.out.flush().get();
    BOOST_CHECK(wait_for([&] { return s.puts == 1; }));

    auto write_f = b.out.write(sstring(8, 'x'));
    seastar::yield().get();
    BOOST_CHECK(!write_f.available());

    s.hold_put = false;
    s.complete_put(std::make_exception_ptr(std::runtime_error("put failed")));
    write_f.get();
    BOOST_CHECK_EQUAL(s.puts, 2u);
    BOOST_CHECK_EQUAL(s.flushes, 0u);
    BOOST_CHECK_EQUAL(s.flush_errors, 1u);
    BOOST_CHECK_THROW(b.out.flush().get(), std::runtime_error);
    b.close();
    BOOST_CHECK_EQUAL(s.flush_errors, 1u);
}

// Simple (mainly compilation) test for basic_memory_data_sink implementation over standard collections
template <template <typename T> class Col>
void do_test_memory_data_sink() {
    using Collection = Col<temporary_buffer<char>>;
    Collection col;
    auto s = data_sink(std::make_unique<util::basic_memory_data_sink<Collection>>(col));
    for (unsigned i = 0; i < 3; i++) {
        s.put(temporary_buffer<char>::copy_of(fmt::to_string(i))).get();
    }
    BOOST_REQUIRE_EQUAL(col.size(), 3);
    auto it = col.begin();
    for (unsigned i = 0; i < 3; i++) {
        BOOST_REQUIRE_EQUAL(internal::to_sstring<std::string>(*it), fmt::to_string(i));
        it++;
    }
}

SEASTAR_THREAD_TEST_CASE(test_memory_data_sink) {
    do_test_memory_data_sink<std::vector>();
    do_test_memory_data_sink<std::list>();
    do_test_memory_data_sink<std::deque>();
}

#ifdef SEASTAR_DATA_SINK_PUT_DEBUG

// Hands out a future the test controls, so a caller that drops it leaves the put
// in flight for as long as the sink lives.
class pending_put_sink final : public data_sink_impl {
    promise<>& _pending;
public:
    explicit pending_put_sink(promise<>& pending) noexcept : _pending(pending) { }

    future<> put(std::span<temporary_buffer<char>> bufs) override {
        return _pending.get_future();
    }

    future<> close() override { return make_ready_future<>(); }
    size_t buffer_size() const noexcept override { return 4096; }
};

class throwing_put_sink final : public data_sink_impl {
public:
    future<> put(std::span<temporary_buffer<char>> bufs) override {
        throw std::runtime_error("put failed");
    }

    future<> close() override { return make_ready_future<>(); }
    size_t buffer_size() const noexcept override { return 4096; }
};

// The abandoned put the assert catches aborts the process, so it has no test case
// here; what these two cover is the other half, that draining a put and failing to
// issue one both leave the sink destructible.

// A put that throws unwinds the guard, which clears the flag again.
SEASTAR_THREAD_TEST_CASE(test_throwing_put_leaves_sink_destructible) {
    data_sink sink(std::make_unique<throwing_put_sink>());
    auto f = sink.put(temporary_buffer<char>(8));
    BOOST_REQUIRE_THROW(f.get(), std::runtime_error);
}

SEASTAR_THREAD_TEST_CASE(test_drained_put_leaves_sink_destructible) {
    promise<> pending;
    data_sink sink(std::make_unique<pending_put_sink>(pending));
    auto f = sink.put(temporary_buffer<char>(8));
    pending.set_value();
    f.get();
}

#endif
