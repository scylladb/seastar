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

#include <seastar/core/iostream.hh>
#include <seastar/testing/thread_test_case.hh>
#include <functional>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace seastar;

// A data_source_impl that delivers a pre-split sequence of temporary_buffers,
// and records whether get() is called a second time after returning the EOF
// (empty) buffer.
class tracking_data_source_impl final : public data_source_impl {
    std::vector<temporary_buffer<char>> _bufs;
    size_t _idx = 0;
    bool _eof_returned = false;
    bool _called_after_eof = false;
public:
    explicit tracking_data_source_impl(std::vector<temporary_buffer<char>> bufs)
        : _bufs(std::move(bufs)) {}

    future<temporary_buffer<char>> get() override {
        if (_eof_returned) {
            _called_after_eof = true;
        }
        if (_idx >= _bufs.size()) {
            _eof_returned = true;
            return make_ready_future<temporary_buffer<char>>();
        }
        return make_ready_future<temporary_buffer<char>>(std::move(_bufs[_idx++]));
    }

    bool called_after_eof() const noexcept { return _called_after_eof; }
};

// -----------------------------------------------------------------------
// Parameters
// -----------------------------------------------------------------------

// 8 total bytes, non-power-of-two.
static constexpr size_t TOTAL_DATA = 8;

// Max source chunk size. Chosen to be > 2 * FIXED_OP_N so that a single source
// buffer can be consumed by at least two consecutive read operations.
static constexpr size_t MAX_SOURCE_CHUNK = 7;

// Fixed N for parameterised operations (read_up_to, read_exactly, skip).
// Non-power-of-two, doesn't divide evenly into TOTAL_DATA or MAX_SOURCE_CHUNK.
static constexpr size_t FIXED_OP_N = 3;

// Maximum operation sequence length.
static constexpr size_t MAX_OPS = 4;

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

// Returns a string of `len` bytes filled with a cycling 'a'..'z' pattern so
// that data-integrity failures produce readable diffs.
static std::string make_data(size_t len) {
    std::string s(len, '\0');
    for (size_t i = 0; i < len; i++) {
        s[i] = 'a' + (i % 26);
    }
    return s;
}

enum class op_type { read, read_up_to, read_exactly, skip };

static const op_type all_ops[] = {
    op_type::read, op_type::read_up_to, op_type::read_exactly, op_type::skip
};

static std::string_view op_name(op_type op) {
    switch (op) {
    case op_type::read:         return "read";
    case op_type::read_up_to:   return "read_up_to";
    case op_type::read_exactly: return "read_exactly";
    case op_type::skip:         return "skip";
    }
    return "?";
}

static std::string format_context(const std::vector<size_t>& chunks,
                                   const std::vector<op_type>& ops) {
    std::ostringstream os;
    os << "source_chunks=[";
    for (size_t i = 0; i < chunks.size(); i++) {
        if (i) os << ",";
        os << chunks[i];
    }
    os << "] ops=[";
    for (size_t i = 0; i < ops.size(); i++) {
        if (i) os << ",";
        os << op_name(ops[i]);
        if (ops[i] != op_type::read) {
            os << "(" << FIXED_OP_N << ")";
        }
    }
    os << "]";
    return os.str();
}

// Calls fn(chunks) for every ordered partition of `remaining` into parts of
// size 1..max_part (i.e. every composition of `remaining` with bounded parts).
template <typename Fn>
static void for_each_composition(size_t remaining, size_t max_part,
                                  std::vector<size_t>& current, Fn&& fn) {
    if (remaining == 0) {
        fn(std::as_const(current));
        return;
    }
    for (size_t sz = 1; sz <= std::min(remaining, max_part); sz++) {
        current.push_back(sz);
        for_each_composition(remaining - sz, max_part, current, fn);
        current.pop_back();
    }
}

// Calls fn(ops) for every sequence of 1..max_len operations drawn from all_ops.
template <typename Fn>
static void for_each_op_sequence(size_t max_len, Fn&& fn) {
    std::vector<op_type> seq;
    std::function<void()> recurse = [&]() {
        if (!seq.empty()) {
            fn(std::as_const(seq));
        }
        if (seq.size() < max_len) {
            for (auto op : all_ops) {
                seq.push_back(op);
                recurse();
                seq.pop_back();
            }
        }
    };
    recurse();
}

// Checks the invariants after executing an operation sequence:
//
//   1. No data loss:        every non-skipped byte from the source appears in
//                           the output, in order.
//   2. No duplication:      no byte appears more than once.
//   3. No reads past EOF:   data_source::get() is never called after it
//                           returned an empty (EOF) buffer.
//   4. read_up_to(n):       never returns more than n bytes.
//   5. read_exactly(n):     returns exactly n bytes, or fewer only if the
//                           stream is at EOF immediately after.
//   6. skip(n):             skips exactly min(n, remaining) bytes; the next
//                           read returns the correct continuation.
//
// Invariants 1, 2, and 6 are checked implicitly via a cursor (`pos`) into the
// known source data: each returned buffer is compared against data[pos..],
// and pos is advanced by the actual number of bytes consumed (or skipped).
// After the operation sequence the remaining stream is drained and its tail
// is compared against data[pos..], catching any under- or over-skip.
static void run_sequence(const std::string& data,
                         const std::vector<size_t>& chunks,
                         const std::vector<op_type>& ops) {
    std::vector<temporary_buffer<char>> bufs;
    for (size_t pos = 0; size_t sz : chunks) {
        temporary_buffer<char> buf(sz);
        std::copy_n(data.data() + pos, sz, buf.get_write());
        pos += sz;
        bufs.push_back(std::move(buf));
    }
    auto* raw = new tracking_data_source_impl(std::move(bufs));
    auto& src = *raw;
    auto in = input_stream<char>(data_source(std::unique_ptr<data_source_impl>(raw)));

    size_t pos = 0;
    bool stream_ended = false;
    const std::string ctx = format_context(chunks, ops);
    fmt::print("run {}\n", ctx);

    for (auto op : ops) {
        if (stream_ended) {
            break;
        }

        switch (op) {
        case op_type::read: {
            auto buf = in.read().get();
            if (buf.empty()) {
                BOOST_REQUIRE_MESSAGE(pos == data.size(),
                    "read() returned EOF before all data was consumed: " << ctx);
                stream_ended = true;
            } else {
                BOOST_REQUIRE_MESSAGE(pos + buf.size() <= data.size(),
                    "read() returned more bytes than available: " << ctx);
                BOOST_REQUIRE_MESSAGE(
                    std::string_view(buf.get(), buf.size()) ==
                    std::string_view(data.data() + pos, buf.size()),
                    "read() returned wrong bytes at pos=" << pos << ": " << ctx);
                pos += buf.size();
            }
            break;
        }

        case op_type::read_up_to: {
            auto buf = in.read_up_to(FIXED_OP_N).get();
            if (buf.empty()) {
                BOOST_REQUIRE_MESSAGE(pos == data.size(),
                    "read_up_to() returned EOF before all data was consumed: " << ctx);
                stream_ended = true;
            } else {
                BOOST_REQUIRE_MESSAGE(buf.size() <= FIXED_OP_N,
                    "read_up_to(" << FIXED_OP_N << ") returned " << buf.size()
                    << " bytes (more than requested): " << ctx);
                BOOST_REQUIRE_MESSAGE(pos + buf.size() <= data.size(),
                    "read_up_to() returned more bytes than available: " << ctx);
                BOOST_REQUIRE_MESSAGE(
                    std::string_view(buf.get(), buf.size()) ==
                    std::string_view(data.data() + pos, buf.size()),
                    "read_up_to() returned wrong bytes at pos=" << pos << ": " << ctx);
                pos += buf.size();
            }
            break;
        }

        case op_type::read_exactly: {
            auto buf = in.read_exactly(FIXED_OP_N).get();
            size_t returned = buf.size();
            BOOST_REQUIRE_MESSAGE(pos + returned <= data.size(),
                "read_exactly() returned more bytes than available: " << ctx);
            BOOST_REQUIRE_MESSAGE(
                std::string_view(buf.get(), returned) ==
                std::string_view(data.data() + pos, returned),
                "read_exactly() returned wrong bytes at pos=" << pos << ": " << ctx);
            pos += returned;
            if (returned < FIXED_OP_N) {
                // Partial result is only valid at the true end of stream.
                auto eof_buf = in.read().get();
                BOOST_REQUIRE_MESSAGE(eof_buf.empty(),
                    "read_exactly() returned " << returned << " < " << FIXED_OP_N
                    << " bytes but stream is not at EOF: " << ctx);
                stream_ended = true;
            }
            break;
        }

        case op_type::skip: {
            // skip(n) throws if the stream has fewer than n bytes remaining.
            size_t remaining = data.size() - pos;
            if (FIXED_OP_N > remaining) {
                BOOST_REQUIRE_THROW(in.skip(FIXED_OP_N).get(), std::runtime_error);
                // Stream is in an error state; the caller is expected not to
                // use it further.
                stream_ended = true;
            } else {
                in.skip(FIXED_OP_N).get();
                pos += FIXED_OP_N;
            }
            break;
        }
        }
    }

    // Drain whatever the operation sequence left unconsumed and verify it
    // matches the expected tail.  This also validates skip() correctness: any
    // over- or under-skip would show up here as a data mismatch.
    if (!stream_ended) {
        std::string tail;
        while (true) {
            auto buf = in.read().get();
            if (buf.empty()) {
                break;
            }
            tail.append(buf.get(), buf.size());
        }
        BOOST_REQUIRE_MESSAGE(tail == data.substr(pos),
            "tail after op sequence doesn't match expected data[" << pos << "..]: " << ctx);
    }

    in.close().get();

    BOOST_REQUIRE_MESSAGE(!src.called_after_eof(),
        "data_source::get() was called after returning EOF: " << ctx);
}

// Verify that data_source_impl::skip(n), when n equals the exact number of
// remaining bytes, does NOT call get() after the source has returned EOF.
//
// The concern: skip() loops calling get() to consume and discard buffers.
// If it skips exactly to EOF, the last get() might return the EOF sentinel
// (empty buffer), consuming it.  A subsequent get() from outside skip()
// would then be calling get() a second time after EOF, which violates the
// data_source_impl contract.
SEASTAR_THREAD_TEST_CASE(test_data_source_skip_to_eof) {
    auto test = [](std::vector<size_t> chunk_sizes) {
        const size_t total = std::accumulate(chunk_sizes.begin(),
                                              chunk_sizes.end(), size_t(0));
        std::vector<temporary_buffer<char>> bufs;
        for (size_t sz : chunk_sizes) {
            temporary_buffer<char> buf(sz);
            std::fill_n(buf.get_write(), sz, 'x');
            bufs.push_back(std::move(buf));
        }

        auto* raw = new tracking_data_source_impl(std::move(bufs));
        auto& tracker = *raw;
        auto ds = data_source(std::unique_ptr<data_source_impl>(raw));

        // Skip exactly to EOF.
        auto leftover = ds.skip(total).get();

        // The leftover should be empty (we skipped all data).
        BOOST_REQUIRE_MESSAGE(leftover.empty(),
            "skip(total) should return an empty buffer when skipping to exact EOF");

        // Key assertion: skip() must not have consumed the EOF sentinel.
        BOOST_REQUIRE_MESSAGE(!tracker.called_after_eof(),
            "data_source_impl::skip() called get() after EOF was returned");

        // Now call get() once — this should be the FIRST EOF return.
        auto eof_buf = ds.get().get();
        BOOST_REQUIRE(eof_buf.empty());
        BOOST_REQUIRE_MESSAGE(!tracker.called_after_eof(),
            "First get() after skip-to-EOF triggered called_after_eof");

        ds.close().get();
    };

    // Empty stream: skip(0) should be a no-op, but the current
    // implementation unconditionally calls get(), consuming the EOF
    // sentinel and then throwing "premature end of stream".
    test({});

    test({8});
    test({4, 4});
    test({3, 3, 2});
    test({1, 1, 1, 1, 1, 1, 1, 1});
    test({7, 1});
    test({1, 7});
    test({2, 3, 3});
}

// Iterates over all compositions of TOTAL_DATA with parts 1..MAX_SOURCE_CHUNK
// (~127 source patterns) crossed with all op sequences of length 1..MAX_OPS
// over 4 op types (~340 sequences), giving ~43k cases in total.
SEASTAR_THREAD_TEST_CASE(test_read_invariants) {
    const std::string data = make_data(TOTAL_DATA);
    std::vector<size_t> chunk_pattern;
    for_each_composition(TOTAL_DATA, MAX_SOURCE_CHUNK, chunk_pattern,
            [&](const std::vector<size_t>& chunks) {
        for_each_op_sequence(MAX_OPS, [&](const std::vector<op_type>& ops) {
            run_sequence(data, chunks, ops);
        });
    });
}
