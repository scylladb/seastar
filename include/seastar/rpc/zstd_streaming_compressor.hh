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
 * Copyright (C) 2023 Scylladb, Ltd.
 */

#pragma once

#include <seastar/core/sstring.hh>
#include <seastar/rpc/rpc_types.hh>
#include <zstd.h>

namespace seastar {
namespace rpc {

struct zstd_cstream_deleter {
    void operator()(ZSTD_CStream* stream) const noexcept {
        ZSTD_freeCStream(stream);
    }
};

struct zstd_dstream_deleter {
    void operator()(ZSTD_DStream* stream) const noexcept {
        ZSTD_freeDStream(stream);
    }
};

class zstd_streaming_compressor final : public compressor {
    std::unique_ptr<ZSTD_CStream, zstd_cstream_deleter> _cstream;
    std::unique_ptr<ZSTD_DStream, zstd_dstream_deleter> _dstream;
    void check_zstd(size_t ret, const char* text) {
        if (ZSTD_isError(ret)) {
            throw std::runtime_error(fmt::format("{} error: {}", text, ZSTD_getErrorName(ret)));
        }
    }
public:
    zstd_streaming_compressor()
        : _cstream(ZSTD_createCStream())
        , _dstream(ZSTD_createDStream())
    {
        check_zstd(ZSTD_initCStream(_cstream.get(), 1), "ZSTD_initCStream(.., 1)");
        check_zstd(ZSTD_CCtx_setParameter(_cstream.get(), ZSTD_c_windowLog, 16), "ZSTD_CCtx_setParameter(.., ZSTD_c_windowLog, 16)");
        check_zstd(ZSTD_initDStream(_dstream.get()), "ZSTD_initDStream");
    }
    class factory final : public rpc::compressor::factory {
    public:
        virtual const sstring& supported() const override;
        virtual std::unique_ptr<rpc::compressor> negotiate(sstring feature, bool is_server) const override;
    };
public:
    virtual snd_buf compress(size_t head_space, snd_buf data) override;
    virtual rcv_buf decompress(rcv_buf data) override;
    sstring name() const override;
};

}
}
