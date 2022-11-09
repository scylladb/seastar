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
 * Copyright (C) 2019 Elazar Leibovich
 */

#pragma once

#include <seastar/core/memory.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/util/std-compat.hh>

namespace seastar {

/// Creates a `temporary_buffer` allocated by a custom allocator
///
/// \param allocator allocator to use when allocating the temporary_buffer
/// \param size      size of the temporary buffer
template <typename CharType>
temporary_buffer<CharType> make_temporary_buffer(std::pmr::polymorphic_allocator<CharType>* allocator, std::size_t size) {
    if (allocator == memory::malloc_allocator) {
        return temporary_buffer<CharType>(size);
    }
    CharType *buffer = allocator->allocate(size);
    return temporary_buffer<CharType>(buffer, size,
        make_deleter(deleter(), [allocator, buffer, size] () mutable { allocator->deallocate(buffer, size); }));
}

}
