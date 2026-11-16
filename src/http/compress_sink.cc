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

#include "compress_sink.hh"

#if defined(SEASTAR_HAVE_ZLIB) || defined(SEASTAR_HAVE_ZSTD)

#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/net/packet.hh>

#include <algorithm>
#include <exception>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>

#ifdef SEASTAR_HAVE_ZLIB
#include <zlib.h>
#endif
#ifdef SEASTAR_HAVE_ZSTD
#include <zstd.h>
#endif

namespace seastar::http::internal {

namespace {

// The most that is compressed, and the most that is produced, in one call
// into the compression library. It bounds the time spent between two
// preemption points.
constexpr size_t compress_chunk_size = 32 * 1024;

// The fastest level of both libraries. The data is compressed on the reactor
// thread, and the higher levels buy little for the text this is used on.
constexpr int compression_level = 1;

// A codec compresses [in, in + in_len) into [out, out + out_len), advances
// \c in and \c in_len past what it consumed and returns how much it produced.
// With \c finish set, it ends the compressed stream and sets \c done once
// all of it has been produced.

#ifdef SEASTAR_HAVE_ZLIB
class gzip_codec {
    z_stream _zs{};
public:
    gzip_codec() {
        // 15 is the default window size, adding 16 selects the gzip wrapper
        // over the zlib one.
        if (deflateInit2(&_zs, compression_level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            throw std::bad_alloc();
        }
    }
    gzip_codec(const gzip_codec&) = delete;
    ~gzip_codec() {
        deflateEnd(&_zs);
    }
    size_t compress(const char*& in, size_t& in_len, char* out, size_t out_len, bool finish, bool& done) {
        _zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in));
        _zs.avail_in = in_len;
        _zs.next_out = reinterpret_cast<Bytef*>(out);
        _zs.avail_out = out_len;
        int r = deflate(&_zs, finish ? Z_FINISH : Z_NO_FLUSH);
        // Z_BUF_ERROR only says that no progress was possible, which happens
        // when there is nothing to consume
        if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) {
            throw std::runtime_error("gzip compression failed");
        }
        in += in_len - _zs.avail_in;
        in_len = _zs.avail_in;
        done = r == Z_STREAM_END;
        return out_len - _zs.avail_out;
    }
};
#endif

#ifdef SEASTAR_HAVE_ZSTD
class zstd_codec {
    ZSTD_CCtx* _ctx;

    void set(ZSTD_cParameter param, int value) {
        if (ZSTD_isError(ZSTD_CCtx_setParameter(_ctx, param, value))) {
            ZSTD_freeCCtx(_ctx);
            throw std::runtime_error("failed to configure zstd compression");
        }
    }
public:
    zstd_codec() : _ctx(ZSTD_createCCtx()) {
        if (!_ctx) {
            throw std::bad_alloc();
        }
        set(ZSTD_c_compressionLevel, compression_level);
        // A 128kB window. It bounds the memory a stream allocates, which
        // otherwise grows with the level's default window.
        set(ZSTD_c_windowLog, 17);
    }
    zstd_codec(const zstd_codec&) = delete;
    ~zstd_codec() {
        ZSTD_freeCCtx(_ctx);
    }
    size_t compress(const char*& in, size_t& in_len, char* out, size_t out_len, bool finish, bool& done) {
        ZSTD_inBuffer ib{in, in_len, 0};
        ZSTD_outBuffer ob{out, out_len, 0};
        // Flushing ends the block at the end of the input. Otherwise the
        // library collects input up to its block size, which is larger than
        // what it is given at a time, and compresses it all in one call.
        size_t r = ZSTD_compressStream2(_ctx, &ob, &ib, finish ? ZSTD_e_end : ZSTD_e_flush);
        if (ZSTD_isError(r)) {
            throw std::runtime_error(std::string("zstd compression failed: ") + ZSTD_getErrorName(r));
        }
        in += ib.pos;
        in_len -= ib.pos;
        done = finish && r == 0;
        return ob.pos;
    }
};
#endif

template <typename Codec>
class compress_sink_impl final : public data_sink_impl {
    output_stream<char> _out;
    Codec _codec;
    std::unique_ptr<char[]> _obuf = std::make_unique_for_overwrite<char[]>(compress_chunk_size);
    // Set when compressing or writing failed. The compressed stream is then
    // broken, and the codec may be in a state in which it must not be used.
    bool _failed = false;

    future<> do_put(temporary_buffer<char> buf) {
        const char* in = buf.get();
        size_t left = buf.size();
        bool done;
        try {
            while (left) {
                // Hand the library a bounded slice whatever the size of the
                // buffer, so a large buffer cannot stall the reactor.
                size_t in_len = std::min(left, compress_chunk_size);
                left -= in_len;
                while (in_len) {
                    size_t n = _codec.compress(in, in_len, _obuf.get(), compress_chunk_size, false, done);
                    if (n) {
                        co_await _out.write(_obuf.get(), n);
                    }
                }
                co_await coroutine::maybe_yield();
            }
        } catch (...) {
            _failed = true;
            throw;
        }
    }

    future<> finish() {
        const char* in = nullptr;
        size_t in_len = 0;
        bool done = false;
        while (!done) {
            size_t n = _codec.compress(in, in_len, _obuf.get(), compress_chunk_size, true, done);
            if (n) {
                co_await _out.write(_obuf.get(), n);
            }
        }
    }
public:
    explicit compress_sink_impl(output_stream<char>&& out) : _out(std::move(out)) {}

#if SEASTAR_API_LEVEL >= 9
    future<> put(std::span<temporary_buffer<char>> data) override {
        return data_sink_impl::fallback_put(data, [this] (temporary_buffer<char>&& buf) {
            return do_put(std::move(buf));
        });
    }
#else
    future<> put(net::packet data) override {
        return data_sink_impl::fallback_put(std::move(data));
    }
    using data_sink_impl::put;
    future<> put(temporary_buffer<char> buf) override {
        return do_put(std::move(buf));
    }
#endif

    future<> flush() override {
        // Only what the library has already produced is flushed. Forcing it
        // to emit what it holds would cost compression ratio on every flush.
        return _out.flush();
    }

    future<> close() override {
        std::exception_ptr ex;
        if (!_failed) {
            try {
                co_await finish();
            } catch (...) {
                ex = std::current_exception();
            }
        }
        // The stream must be closed even if the compressed stream was not ended
        co_await _out.close();
        if (ex) {
            std::rethrow_exception(std::move(ex));
        }
    }
};

template <typename Codec>
output_stream<char> make_compress_output_stream(output_stream<char>&& out) {
    return output_stream<char>(data_sink(std::make_unique<compress_sink_impl<Codec>>(std::move(out))), compress_chunk_size);
}

}

#ifdef SEASTAR_HAVE_ZLIB
output_stream<char> make_gzip_output_stream(output_stream<char>&& out) {
    return make_compress_output_stream<gzip_codec>(std::move(out));
}
#endif

#ifdef SEASTAR_HAVE_ZSTD
output_stream<char> make_zstd_output_stream(output_stream<char>&& out) {
    return make_compress_output_stream<zstd_codec>(std::move(out));
}
#endif

}

#endif
