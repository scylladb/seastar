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
 * Copyright 2016 ScyllaDB
 */

#pragma once

#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <iosfwd>
#include <boost/container/static_vector.hpp>

#include "core/sstring.hh"

namespace seastar {

struct shared_object {
    sstring name;
    uintptr_t begin;
    uintptr_t end; // C++-style, last addr + 1
};

struct frame {
    const shared_object* so;
    uintptr_t addr;
};

bool operator==(const frame& a, const frame& b);


// If addr doesn't seem to belong to any of the provided shared objects, it
// will be considered as part of the executable.
frame decorate(uintptr_t addr);

// Invokes func for each frame passing it as argument.
template<typename Func>
void backtrace(Func&& func) noexcept(noexcept(func(frame()))) {
    unw_context_t context;
    if (unw_getcontext(&context) < 0) {
        return;
    }

    unw_cursor_t cursor;
    if (unw_init_local(&cursor, &context) < 0) {
        return;
    }

    while (unw_step(&cursor) > 0) {
        unw_word_t ip;
        if (unw_get_reg(&cursor, UNW_REG_IP, &ip) < 0) {
            break;
        }
        if (!ip) {
            break;
        }
        func(decorate(ip - 1));
    }
}

class saved_backtrace {
public:
    using vector_type = boost::container::static_vector<frame, 64>;
private:
    vector_type _frames;
public:
    saved_backtrace() = default;
    saved_backtrace(vector_type f) : _frames(std::move(f)) {}
    size_t hash() const;

    friend std::ostream& operator<<(std::ostream& out, const saved_backtrace&);

    bool operator==(const saved_backtrace& o) const {
        return _frames == o._frames;
    }

    bool operator!=(const saved_backtrace& o) const {
        return !(*this == o);
    }
};

}

namespace std {

template<>
struct hash<seastar::saved_backtrace> {
    size_t operator()(const seastar::saved_backtrace& b) const {
        return b.hash();
    }
};

}

namespace seastar {

saved_backtrace current_backtrace() noexcept;
std::ostream& operator<<(std::ostream& out, const saved_backtrace& b);

}
