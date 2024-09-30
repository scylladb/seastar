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
 * Copyright (C) 2016 Scylladb, Ltd.
 */

#pragma once

#include <seastar/core/sstring.hh>
#include <seastar/rpc/rpc_types.hh>

namespace seastar {

namespace rpc {

// This is meta compressor factory. It gets an array of regular factories that
// support one compression algorithm each and negotiates common compression algorithm
// that is supported both by a client and a server. The order of algorithm preferences
// is the order they appear in clien's list
class multi_algo_compressor_factory : public rpc::compressor::factory {
    std::vector<const rpc::compressor::factory*> _factories;
    sstring _features;

public:
    multi_algo_compressor_factory(std::vector<const rpc::compressor::factory*> factories);
    multi_algo_compressor_factory(std::initializer_list<const rpc::compressor::factory*> factories) :
        multi_algo_compressor_factory(std::vector<const rpc::compressor::factory*>(std::move(factories))) {}
    multi_algo_compressor_factory(const rpc::compressor::factory* factory) : multi_algo_compressor_factory({factory}) {}
    // return feature string that will be sent as part of protocol negotiation
    const sstring& supported() const override {
        return _features;
    }
    // negotiate compress algorithm
    std::unique_ptr<compressor> negotiate(sstring feature, bool is_server) const override {
        return negotiate(feature, is_server, nullptr);
    }
    std::unique_ptr<compressor> negotiate(sstring feature, bool is_server, std::function<future<>()> send_empty_frame) const override;
};

}

}
