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
 * Copyright (C) 2024 ScyllaDB
 */

#include "create_file.hh"

#include <cstdint>
#include <cstring>

#include <boost/range/irange.hpp>

#include <seastar/core/aligned_buffer.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/file.hh>
#include <seastar/core/seastar.hh>
#include <seastar/util/file.hh>

using namespace seastar;

namespace {

auto allocate_buffer(size_t buffer_size) {
    constexpr size_t alignment{4096u};
    auto buffer = allocate_aligned_buffer<char>(buffer_size, alignment);

    std::memset(buffer.get(), 'X', buffer_size);
    return buffer;
}

} // namespace

namespace seastar::testing {

future<> create_file_with_size(std::string_view file_path, size_t total_size) {
    return open_file_dma(file_path, open_flags::rw | open_flags::create, file_open_options{}).then([total_size] (file f) mutable {
        if (total_size == 0u) {
            return f.close();
        }

        return do_with(std::move(f), [total_size] (auto& f) mutable {
            return f.truncate(total_size).then([f, total_size] () mutable {
                const size_t buffer_size = 4096u;
                const uint64_t additional_write_needed = (total_size % buffer_size) != 0;
                const uint64_t buffers_count = (total_size / buffer_size) + (additional_write_needed ? 1u : 0u);

                return do_with(boost::irange(UINT64_C(0), buffers_count), [f, buffer_size] (auto& buffers_range) mutable {
                    return max_concurrent_for_each(buffers_range.begin(), buffers_range.end(), 64, [f, buffer_size] (auto buffer_id) mutable {
                        auto source_buffer = allocate_buffer(buffer_size);
                        auto write_position = buffer_id * buffer_size;
                        return do_with(std::move(source_buffer), [f, write_position, buffer_size] (const auto& buffer) mutable {
                            return f.dma_write(write_position, buffer.get(), buffer_size).discard_result();
                        });
                    });
                }).then([f] () mutable {
                    return f.close();
                });
            });
        });
    });
}

} // namespace seastar::testing
