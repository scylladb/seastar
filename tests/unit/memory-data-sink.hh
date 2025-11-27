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
 * Copyright 2025 ScyllaDB
 */

#pragma once
#include <seastar/util/memory-data-sink.hh>

namespace seastar {

inline void append_buffers(std::stringstream& ss, std::span<seastar::temporary_buffer<char>> bufs) {
    for (auto& buf : bufs) {
        ss.write(buf.get(), buf.size());
    }
}

namespace testing {

using memory_data_sink_impl = util::basic_memory_data_sink<std::stringstream>;

/*!
 * \brief a helper data sink that stores everything it gets in a stringstream
 */

class memory_data_sink : public data_sink {
public:
    memory_data_sink(std::stringstream& ss)
        : data_sink(std::make_unique<memory_data_sink_impl>(ss)) {}
};

} // testing namespace
} // seastar namespace
