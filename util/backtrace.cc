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
 * Copyright 2017 ScyllaDB
 */
#include "backtrace.hh"

#include "core/print.hh"


namespace seastar {

saved_backtrace current_backtrace() noexcept {
    saved_backtrace::vector_type v;
    backtrace([&] (uintptr_t addr) {
        if (v.size() < v.capacity()) {
            v.push_back(addr);
        }
    });
    return saved_backtrace(std::move(v));
}

size_t saved_backtrace::hash() const {
    size_t h = 0;
    for (auto addr : _frames) {
        h = ((h << 5) - h) ^ addr;
    }
    return h;
}

std::ostream& operator<<(std::ostream& out, const saved_backtrace& b) {
    bool first = true;
    for (auto addr : b._frames) {
        if (!first) {
            out << ", ";
        }
        out << sprint("0x%x", addr - 1);
        first = false;
    }
    return out;
}

} // namespace seastar
