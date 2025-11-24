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
#pragma once

#include <seastar/util/memory-data-source.hh>
#include <seastar/net/packet.hh>
#include <seastar/core/iostream.hh>

namespace seastar {

namespace net {

class [[deprecated("Use util::memory_data_source")]] packet_data_source final : public data_source_impl {
    util::memory_data_source _mds;

public:
    explicit packet_data_source(net::packet&& p)
        : _mds(p.release())
    {}

    virtual future<temporary_buffer<char>> get() override {
        return _mds.get();
    }
};

[[deprecated("Use util::as_input_stream")]]
static inline
input_stream<char> as_input_stream(packet&& p) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    return input_stream<char>(data_source(std::make_unique<packet_data_source>(std::move(p))));
#pragma GCC diagnostic pop
}

}

}
