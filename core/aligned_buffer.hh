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
 * Copyright (C) 2016 ScyllaDB.
 */
#pragma once
#include <stdlib.h>
#include <memory>
#include <stdexcept>
#include "print.hh"

namespace seastar {


struct free_deleter {
    void operator()(void* p) { ::free(p); }
};

template <typename CharType>
inline
std::unique_ptr<CharType[], free_deleter> allocate_aligned_buffer(size_t size, size_t align) {
    static_assert(sizeof(CharType) == 1, "must allocate byte type");
    void* ret;
    auto r = posix_memalign(&ret, align, size);
    if (r == ENOMEM) {
        throw std::bad_alloc();
    } else if (r == EINVAL) {
        throw std::runtime_error(sprint("Invalid alignment of %d; allocating %d bytes", align, size));
    } else {
        assert(r == 0);
        return std::unique_ptr<CharType[], free_deleter>(reinterpret_cast<CharType *>(ret));
    }
}


}
