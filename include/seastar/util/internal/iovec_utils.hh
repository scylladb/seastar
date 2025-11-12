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
 * Copyright (C) 2022 ScyllaDB.
 */

#include <span>

#include <sys/types.h>

#pragma once

namespace seastar {
namespace internal {

inline size_t iovec_len(const iovec* begin, size_t len)
{
    size_t ret = 0;
    auto end = begin + len;
    while (begin != end) {
        ret += begin++->iov_len;
    }
    return ret;
}


inline size_t iovec_len(std::span<const iovec> iov) {
    size_t ret = 0;
    for (auto&& e : iov) {
        ret += e.iov_len;
    }
    return ret;
}

// Skips first \size bytes from the data \iov points to
// Can update some iovecs in-place
inline std::span<iovec> iovec_trim_front(std::span<iovec> iov, size_t size) {
    unsigned pos = 0;
    while (pos < iov.size() && size > 0) {
        if (iov[pos].iov_len > size) {
            iov[pos].iov_base = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(iov[pos].iov_base) + size);
            iov[pos].iov_len -= size;
            break;
        }

        size -= iov[pos].iov_len;
        pos++;
    }
    return iov.subspan(pos);
}

// Given a properly aligned vector of iovecs, ensures that it respects the
// IOV_MAX limit, by trimming if necessary. The modified vector still satisfied
// the alignment requirements.
// Returns the final total length of all iovecs.
size_t sanitize_iovecs(std::vector<iovec>& iov, size_t disk_alignment) noexcept;

} // internal namespace
} // seastar namespace
