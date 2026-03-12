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
#include <seastar/core/sstring.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/random.hh>
#include <vector>
#include <list>
#include <deque>
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
