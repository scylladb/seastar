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
 * Copyright (C) 2024 ScyllaDB.
 */


#pragma once
#include <seastar/util/trace.hh>

namespace seastar {
class io_desc_read_write;

namespace internal {

template<>
struct event_tracer<trace_event::IO_POLL> {
    static int size() noexcept { return 0; }
    static void put(char* buf) noexcept { }
};

inline uint32_t disguise_request_ptr(const io_desc_read_write& desc) noexcept {
    return reinterpret_cast<uintptr_t>(&desc) >> 3;
}

template<>
struct event_tracer<trace_event::IO_QUEUE> {
    static int size() noexcept {
        return sizeof(uint8_t)   // direction idx
            + sizeof(uint8_t)    // class id
            + sizeof(uint16_t)   // length in blocks
            + sizeof(uint32_t);  // request "id"
    }
    static void put(char* buf, const io_desc_read_write& desc, unsigned class_id, int rw_idx, size_t nr_blocks) noexcept {
        write_le(buf + 0, (uint8_t)rw_idx);
        write_le(buf + 1, (uint8_t)class_id);
        write_le(buf + 2, (uint16_t)nr_blocks); // up to 32Mb
        write_le(buf + 4, disguise_request_ptr(desc));
    }
};

template<>
struct event_tracer<trace_event::IO_DISPATCH> {
    static int size() noexcept {
        return sizeof(uint32_t);  // request "id"
    }
    static void put(char* buf, const io_desc_read_write& desc) noexcept {
        write_le(buf, disguise_request_ptr(desc));
    }
};

template<>
struct event_tracer<trace_event::IO_COMPLETE> {
    static int size() noexcept {
        return sizeof(uint32_t);  // request "id"
    }
    static void put(char* buf, const io_desc_read_write& desc) noexcept {
        write_le(buf, disguise_request_ptr(desc));
    }
};

} // internal namespace
} // seastar namespace
