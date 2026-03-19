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
 * Copyright (C) 2020 ScyllaDB.
 */

#include <seastar/core/future.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/units.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/testing/random.hh>
#include <seastar/http/request.hh>
#include <seastar/util/short_streams.hh>
#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <string_view>

using namespace seastar;
using namespace util;

/*
 * Simple data source producing up to total_size bytes
 * in buffer_size-byte chunks.
 * */
class test_source_impl : public data_source_impl {
    short _current_letter = 0; // a-z corresponds to 0-25
    size_t _buffer_size;
    size_t _remaining_size;
public:
    test_source_impl(size_t buffer_size, size_t total_size)
        : _buffer_size(buffer_size), _remaining_size(total_size) {
    }
    virtual future<temporary_buffer<char>> get() override {
        size_t len = std::min(_buffer_size, _remaining_size);
        temporary_buffer<char> tmp(len);
        for (size_t i = 0; i < len; i++) {
            tmp.get_write()[i] = 'a' + _current_letter;
            ++_current_letter %= 26;
        }
        _remaining_size -= len;
        return make_ready_future<temporary_buffer<char>>(std::move(tmp));
    }
    virtual future<temporary_buffer<char>> skip(uint64_t n) override {
        _remaining_size -= std::min(_remaining_size, n);
        _current_letter += n %= 26;
        return make_ready_future<temporary_buffer<char>>();
    }
};

SEASTAR_TEST_CASE(test_read_all) {
    return async([] {
        auto check_read_all = [] (input_stream<char>& strm, const char* test) {
            auto all = read_entire_stream(strm).get();
            sstring s;
            for (auto&& buf: all) {
                s += seastar::to_sstring(std::move(buf));
            };
            BOOST_REQUIRE_EQUAL(s, test);
        };
        input_stream<char> inp(data_source(std::make_unique<test_source_impl>(5, 15)));
        check_read_all(inp, "abcdefghijklmno");
        BOOST_REQUIRE(inp.eof());
        input_stream<char> inp2(data_source(std::make_unique<test_source_impl>(5, 16)));
        check_read_all(inp2, "abcdefghijklmnop");
        BOOST_REQUIRE(inp2.eof());
        input_stream<char> empty_inp(data_source(std::make_unique<test_source_impl>(5, 0)));
        check_read_all(empty_inp, "");
        BOOST_REQUIRE(empty_inp.eof());

        input_stream<char> inp_cont(data_source(std::make_unique<test_source_impl>(5, 15)));
        BOOST_REQUIRE_EQUAL(to_sstring(read_entire_stream_contiguous(inp_cont).get()), "abcdefghijklmno");
        BOOST_REQUIRE(inp_cont.eof());
        input_stream<char> inp_cont2(data_source(std::make_unique<test_source_impl>(5, 16)));
        BOOST_REQUIRE_EQUAL(to_sstring(read_entire_stream_contiguous(inp_cont2).get()), "abcdefghijklmnop");
        BOOST_REQUIRE(inp_cont2.eof());
        input_stream<char> empty_inp_cont(data_source(std::make_unique<test_source_impl>(5, 0)));
        BOOST_REQUIRE_EQUAL(to_sstring(read_entire_stream_contiguous(empty_inp_cont).get()), "");
        BOOST_REQUIRE(empty_inp_cont.eof());
    });
}

SEASTAR_TEST_CASE(test_skip_all) {
    return async([] {
        input_stream<char> inp(data_source(std::make_unique<test_source_impl>(5, 15)));
        skip_entire_stream(inp).get();
        BOOST_REQUIRE(inp.eof());
        BOOST_REQUIRE(to_sstring(inp.read().get()).empty());
        input_stream<char> inp2(data_source(std::make_unique<test_source_impl>(5, 16)));
        skip_entire_stream(inp2).get();
        BOOST_REQUIRE(inp2.eof());
        BOOST_REQUIRE(to_sstring(inp2.read().get()).empty());
        input_stream<char> empty_inp(data_source(std::make_unique<test_source_impl>(5, 0)));
        skip_entire_stream(empty_inp).get();
        BOOST_REQUIRE(empty_inp.eof());
        BOOST_REQUIRE(to_sstring(empty_inp.read().get()).empty());
    });
}

SEASTAR_THREAD_TEST_CASE(test_read_exactly) {
    const size_t total_size = 22;
    for (size_t bs = 3; bs < total_size; bs++) {
        input_stream<char> in(data_source(std::make_unique<test_source_impl>(5, total_size)));
        size_t total = 0;
        while (true) {
            auto buf = in.read_exactly(bs).get();
            total += buf.size();
            if (buf.size() != bs) {
                BOOST_REQUIRE_LT(buf.size(), bs);
                if (buf.size() != 0) {
                    buf = in.read_exactly(bs).get();
                    BOOST_REQUIRE_EQUAL(buf.size(), 0);
                }
                break;
            }
        }
        BOOST_REQUIRE_EQUAL(total, total_size);
    }
}

// A data source that produces exactly the given content in
// chunk_size-byte pieces, and fails if get() is called after it
// already returned an empty buffer (EOS). This catches callers that
// do not check _eof before re-entering the source.
class strict_source_impl : public data_source_impl {
    std::string _data;
    size_t _pos = 0;
    size_t _chunk_size;
    bool _eos_returned = false;

public:
    strict_source_impl(std::string data, size_t chunk_size) : _data(std::move(data)), _chunk_size(chunk_size) {}

    future<temporary_buffer<char>> get() override {
        BOOST_REQUIRE_MESSAGE(!_eos_returned, "get() called after EOS — caller does not check _eof");
        if (_pos >= _data.size()) {
            _eos_returned = true;
            return make_ready_future<temporary_buffer<char>>();
        }
        auto n = std::min(_chunk_size, _data.size() - _pos);
        temporary_buffer<char> buf(n);
        std::copy_n(_data.data() + _pos, n, buf.get_write());
        _pos += n;
        return make_ready_future<temporary_buffer<char>>(std::move(buf));
    }
};

// Helper: build an input_stream backed by a strict_source_impl.
static input_stream<char> make_strict_stream(const std::string& data, size_t chunk_size) {
    return input_stream<char>(data_source(std::make_unique<strict_source_impl>(data, chunk_size)));
}

// Helper: build a reference string of given size (cycling a-z).
static std::string make_test_data(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) {
        s[i] = 'a' + (i % 26);
    }
    return s;
}

// For each representative source chunk size, build a shuffled sequence
// of read/read_up_to/read_exactly/skip operations with random sizes,
// cycle through it until the 5 MB stream is exhausted while checking
// data integrity, then re-apply the same sequence post-EOF to verify
// nothing gets stuck.
SEASTAR_THREAD_TEST_CASE(test_read_splitting_invariants) {
    static constexpr size_t STREAM_SIZE = 5_MiB;
    static constexpr size_t MAX_READ_SIZE = 128_KiB;
    // How many full {read, read_up_to, read_exactly, skip} cycles to put
    // into the operations container before shuffling.
    static constexpr size_t OP_CYCLES = 256;
    // Number of chunk-size iterations.  Each picks a random chunk_size
    // from a logarithmic band so that early iterations exercise tiny
    // chunks and later ones exercise large ones, covering the full
    // range [1, STREAM_SIZE].
    static constexpr size_t NUM_CHUNK_ITERATIONS = 40; // ~30 sec. execution time
    static const double LOG_MAX = std::log(static_cast<double>(STREAM_SIZE));

    enum class op_t { read, read_up_to, read_exactly, skip };

    struct op_desc {
        op_t type;
        size_t size;
    };

    auto& rng = testing::local_random_engine;
    std::uniform_int_distribution<size_t> size_dist(1, MAX_READ_SIZE);

    auto data = make_test_data(STREAM_SIZE);

    for (size_t iter = 0; iter < NUM_CHUNK_ITERATIONS; iter++) {
        // Logarithmic band for this iteration: e.g. iter 0 → [1,~2],
        // then [~2,~4], ..., up to [..., STREAM_SIZE].
        const double lo = LOG_MAX * static_cast<double>(iter) / static_cast<double>(NUM_CHUNK_ITERATIONS);
        const double hi = LOG_MAX * static_cast<double>(iter + 1) / static_cast<double>(NUM_CHUNK_ITERATIONS);
        std::uniform_real_distribution<double> band_dist(lo, hi);
        size_t chunk_size = std::max<size_t>(1, static_cast<size_t>(std::exp(band_dist(rng))));
        // Fill the container: repeating read, read_up_to, read_exactly, skip.
        std::vector<op_desc> ops;
        for (size_t c = 0; c < OP_CYCLES; c++) {
            ops.push_back({op_t::read, 0});
            ops.push_back({op_t::read_up_to, size_dist(rng)});
            ops.push_back({op_t::read_exactly, size_dist(rng)});
            ops.push_back({op_t::skip, size_dist(rng)});
        }
        std::ranges::shuffle(ops, rng);

        auto in = make_strict_stream(data, chunk_size);
        size_t pos = 0;

        // --- Phase 1: cycle through ops until the stream is exhausted ---
        for (size_t i = 0; !in.eof(); i = (i + 1) % ops.size()) {
            const auto& [type, size] = ops[i];
            size_t remaining = STREAM_SIZE - pos;

            switch (type) {
            case op_t::read: {
                auto buf = in.read().get();
                BOOST_REQUIRE_MESSAGE(buf.size() <= remaining, "read() returned more than remaining, chunk_size=" << chunk_size);
                if (!buf.empty()) {
                    BOOST_REQUIRE_MESSAGE(std::string_view(buf.get(), buf.size()) == std::string_view(data.data() + pos, buf.size()),
                                          "data mismatch in read(), chunk_size=" << chunk_size);
                }
                pos += buf.size();
                break;
            }
            case op_t::read_up_to: {
                auto buf = in.read_up_to(size).get();
                BOOST_REQUIRE_MESSAGE(buf.size() <= size, "read_up_to() returned more than requested, chunk_size=" << chunk_size);
                BOOST_REQUIRE_MESSAGE(buf.size() <= remaining, "read_up_to() returned more than remaining, chunk_size=" << chunk_size);
                if (!buf.empty()) {
                    BOOST_REQUIRE_MESSAGE(std::string_view(buf.get(), buf.size()) == std::string_view(data.data() + pos, buf.size()),
                                          "data mismatch in read_up_to(), chunk_size=" << chunk_size);
                }
                pos += buf.size();
                break;
            }
            case op_t::read_exactly: {
                auto buf = in.read_exactly(size).get();
                BOOST_REQUIRE_MESSAGE(buf.size() <= size, "read_exactly() returned more than requested, chunk_size=" << chunk_size);
                BOOST_REQUIRE_MESSAGE(buf.size() <= remaining, "read_exactly() returned more than remaining, chunk_size=" << chunk_size);
                if (!buf.empty()) {
                    BOOST_REQUIRE_MESSAGE(std::string_view(buf.get(), buf.size()) == std::string_view(data.data() + pos, buf.size()),
                                          "data mismatch in read_exactly(), chunk_size=" << chunk_size);
                }
                pos += buf.size();
                break;
            }
            case op_t::skip: {
                const auto to_skip = std::min(size, remaining);
                in.skip(to_skip).get();
                pos += to_skip;
                break;
            }
            }
        }
        BOOST_REQUIRE_MESSAGE(pos == STREAM_SIZE, "not all data consumed, chunk_size=" << chunk_size);

        // --- Phase 2: re-apply the same ops after EOS ---
        // Every call must return empty / complete without
        // re-entering the exhausted source (strict_source_impl
        // asserts on post-EOS get() calls).
        BOOST_REQUIRE(in.eof());
        for (const auto& [type, size] : ops) {
            switch (type) {
            case op_t::read: {
                auto buf = in.read().get();
                BOOST_REQUIRE_MESSAGE(buf.empty(), "post-EOF read() returned data, chunk_size=" << chunk_size);
                break;
            }
            case op_t::read_up_to: {
                auto buf = in.read_up_to(size).get();
                BOOST_REQUIRE_MESSAGE(buf.empty(), "post-EOF read_up_to() returned data, chunk_size=" << chunk_size);
                break;
            }
            case op_t::read_exactly: {
                auto buf = in.read_exactly(size).get();
                BOOST_REQUIRE_MESSAGE(buf.empty(), "post-EOF read_exactly() returned data, chunk_size=" << chunk_size);
                break;
            }
            case op_t::skip: {
                in.skip(size).get();
                // Must complete without re-entering the source.
                break;
            }
            }
        }
    }
}
