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
 * Copyright (C) 2019 Scylladb, Ltd.
 */

#include <random>

#include <seastar/rpc/lz4_compressor.hh>
#include <seastar/rpc/lz4_fragmented_compressor.hh>

#include <seastar/testing/perf_tests.hh>
#include <seastar/testing/random.hh>

template<typename Compressor>
struct compression {
    static constexpr size_t small_buffer_size = 128;
    static constexpr size_t large_buffer_size = 16 * 1024 * 1024;

private:
    Compressor _compressor;

    seastar::temporary_buffer<char> _small_buffer_random;
    seastar::temporary_buffer<char> _small_buffer_zeroes;

    std::vector<seastar::temporary_buffer<char>> _large_buffer_random;
    std::vector<seastar::temporary_buffer<char>> _large_buffer_zeroes;

    std::vector<seastar::temporary_buffer<char>> _small_compressed_buffer_random;
    std::vector<seastar::temporary_buffer<char>> _small_compressed_buffer_zeroes;

    std::vector<seastar::temporary_buffer<char>> _large_compressed_buffer_random;
    std::vector<seastar::temporary_buffer<char>> _large_compressed_buffer_zeroes;

private:
    static seastar::rpc::rcv_buf get_rcv_buf(std::vector<temporary_buffer<char>>& input) {
        if (input.size() == 1) {
            return seastar::rpc::rcv_buf(input.front().share());
        }
        auto bufs = std::vector<temporary_buffer<char>>{};
        auto total_size = std::accumulate(input.begin(), input.end(), size_t(0),
            [&] (size_t n, temporary_buffer<char>& buf) {
                bufs.emplace_back(buf.share());
                return n + buf.size();
            });
        return seastar::rpc::rcv_buf(std::move(bufs), total_size);
    }

    static seastar::rpc::snd_buf get_snd_buf(std::vector<temporary_buffer<char>>& input) {
        auto bufs = std::vector<temporary_buffer<char>>{};
        auto total_size = std::accumulate(input.begin(), input.end(), size_t(0),
            [&] (size_t n, temporary_buffer<char>& buf) {
                bufs.emplace_back(buf.share());
                return n + buf.size();
            });
        return seastar::rpc::snd_buf(std::move(bufs), total_size);
    }
    static seastar::rpc::snd_buf get_snd_buf(temporary_buffer<char>& input) {
        return seastar::rpc::snd_buf(input.share());
    }

public:
    compression()
        : _small_buffer_random(seastar::temporary_buffer<char>(small_buffer_size))
        , _small_buffer_zeroes(seastar::temporary_buffer<char>(small_buffer_size))
    {
        auto& eng = testing::local_random_engine;
        auto dist = std::uniform_int_distribution<char>();

        std::generate_n(_small_buffer_random.get_write(), small_buffer_size, [&] { return dist(eng); });
        for (auto i = 0u; i < large_buffer_size / seastar::rpc::snd_buf::chunk_size; i++) {
            _large_buffer_random.emplace_back(seastar::rpc::snd_buf::chunk_size);
            std::generate_n(_large_buffer_random.back().get_write(), seastar::rpc::snd_buf::chunk_size, [&] { return dist(eng); });
            _large_buffer_zeroes.emplace_back(seastar::rpc::snd_buf::chunk_size);
            std::fill_n(_large_buffer_zeroes.back().get_write(), seastar::rpc::snd_buf::chunk_size, 0);
        }

        auto rcv = _compressor.compress(0, seastar::rpc::snd_buf(_small_buffer_random.share()));
        if (auto buffer = std::get_if<seastar::temporary_buffer<char>>(&rcv.bufs)) {
            _small_compressed_buffer_random.emplace_back(std::move(*buffer));
        } else {
            _small_compressed_buffer_random
                = std::move(std::get<std::vector<seastar::temporary_buffer<char>>>(rcv.bufs));
        }

        rcv = _compressor.compress(0, seastar::rpc::snd_buf(_small_buffer_zeroes.share()));
        if (auto buffer = std::get_if<seastar::temporary_buffer<char>>(&rcv.bufs)) {
            _small_compressed_buffer_zeroes.emplace_back(std::move(*buffer));
        } else {
            _small_compressed_buffer_zeroes
                = std::move(std::get<std::vector<seastar::temporary_buffer<char>>>(rcv.bufs));
        }

        auto bufs = std::vector<temporary_buffer<char>>{};
        for (auto&& b : _large_buffer_random) {
            bufs.emplace_back(b.clone());
        }
        rcv = _compressor.compress(0, seastar::rpc::snd_buf(std::move(bufs), large_buffer_size));
        if (auto buffer = std::get_if<seastar::temporary_buffer<char>>(&rcv.bufs)) {
            _large_compressed_buffer_random.emplace_back(std::move(*buffer));
        } else {
            _large_compressed_buffer_random
                = std::move(std::get<std::vector<seastar::temporary_buffer<char>>>(rcv.bufs));
        }

        bufs = std::vector<temporary_buffer<char>>{};
        for (auto&& b : _large_buffer_zeroes) {
            bufs.emplace_back(b.clone());
        }
        rcv = _compressor.compress(0, seastar::rpc::snd_buf(std::move(bufs), large_buffer_size));
        if (auto buffer = std::get_if<seastar::temporary_buffer<char>>(&rcv.bufs)) {
            _large_compressed_buffer_zeroes.emplace_back(std::move(*buffer));
        } else {
            _large_compressed_buffer_zeroes
                = std::move(std::get<std::vector<seastar::temporary_buffer<char>>>(rcv.bufs));
        }
    }

    Compressor& compressor() { return _compressor; }

    seastar::rpc::snd_buf small_buffer_random() {
        return get_snd_buf(_small_buffer_random);
    }
    seastar::rpc::snd_buf small_buffer_zeroes() {
        return get_snd_buf(_small_buffer_zeroes);
    }

    seastar::rpc::snd_buf large_buffer_random() {
        return get_snd_buf(_large_buffer_random);
    }
    seastar::rpc::snd_buf large_buffer_zeroes() {
        return get_snd_buf(_large_buffer_zeroes);
    }

    seastar::rpc::rcv_buf small_compressed_buffer_random() {
        return get_rcv_buf(_small_compressed_buffer_random);
    }
    seastar::rpc::rcv_buf small_compressed_buffer_zeroes() {
        return get_rcv_buf(_small_compressed_buffer_zeroes);
    }

    seastar::rpc::rcv_buf large_compressed_buffer_random() {
        return get_rcv_buf(_large_compressed_buffer_random);
    }
    seastar::rpc::rcv_buf large_compressed_buffer_zeroes() {
        return get_rcv_buf(_large_compressed_buffer_zeroes);
    }
};

using lz4 = compression<seastar::rpc::lz4_compressor>;

PERF_TEST_F(lz4, small_random_buffer_compress) {
    perf_tests::do_not_optimize(
        compressor().compress(0, small_buffer_random())
    );
}

PERF_TEST_F(lz4, small_zeroed_buffer_compress) {
    perf_tests::do_not_optimize(
        compressor().compress(0, small_buffer_zeroes())
    );
}

PERF_TEST_F(lz4, large_random_buffer_compress) {
    perf_tests::do_not_optimize(
        compressor().compress(0, large_buffer_random())
    );
}

PERF_TEST_F(lz4, large_zeroed_buffer_compress) {
    perf_tests::do_not_optimize(
        compressor().compress(0, large_buffer_zeroes())
    );
}

PERF_TEST_F(lz4, small_random_buffer_decompress) {
    perf_tests::do_not_optimize(
        compressor().decompress(small_compressed_buffer_random())
    );
}

PERF_TEST_F(lz4, small_zeroed_buffer_decompress) {
    perf_tests::do_not_optimize(
        compressor().decompress(small_compressed_buffer_zeroes())
    );
}

PERF_TEST_F(lz4, large_random_buffer_decompress) {
    perf_tests::do_not_optimize(
        compressor().decompress(large_compressed_buffer_random())
    );
}

PERF_TEST_F(lz4, large_zeroed_buffer_decompress) {
    perf_tests::do_not_optimize(
        compressor().decompress(large_compressed_buffer_zeroes())
    );
}

using lz4_fragmented = compression<seastar::rpc::lz4_fragmented_compressor>;

PERF_TEST_F(lz4_fragmented, small_random_buffer_compress) {
    perf_tests::do_not_optimize(
        compressor().compress(0, small_buffer_random())
    );
}

PERF_TEST_F(lz4_fragmented, small_zeroed_buffer_compress) {
    perf_tests::do_not_optimize(
        compressor().compress(0, small_buffer_zeroes())
    );
}

PERF_TEST_F(lz4_fragmented, large_random_buffer_compress) {
    perf_tests::do_not_optimize(
        compressor().compress(0, large_buffer_random())
    );
}

PERF_TEST_F(lz4_fragmented, large_zeroed_buffer_compress) {
    perf_tests::do_not_optimize(
        compressor().compress(0, large_buffer_zeroes())
    );
}

PERF_TEST_F(lz4_fragmented, small_random_buffer_decompress) {
    perf_tests::do_not_optimize(
        compressor().decompress(small_compressed_buffer_random())
    );
}

PERF_TEST_F(lz4_fragmented, small_zeroed_buffer_decompress) {
    perf_tests::do_not_optimize(
        compressor().decompress(small_compressed_buffer_zeroes())
    );
}

PERF_TEST_F(lz4_fragmented, large_random_buffer_decompress) {
    perf_tests::do_not_optimize(
        compressor().decompress(large_compressed_buffer_random())
    );
}

PERF_TEST_F(lz4_fragmented, large_zeroed_buffer_decompress) {
    perf_tests::do_not_optimize(
        compressor().decompress(large_compressed_buffer_zeroes())
    );
}
