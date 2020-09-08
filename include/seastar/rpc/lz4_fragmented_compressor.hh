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

#pragma once

#include <seastar/core/sstring.hh>
#include <seastar/rpc/rpc_types.hh>

namespace seastar {
namespace rpc {

class lz4_fragmented_compressor final : public compressor {
public:
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
