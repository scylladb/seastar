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
 * Copyright (C) 2026 ScyllaDB
 */

#pragma once

#include <cstddef>

namespace seastar {

/// \addtogroup memory-module
/// @{

namespace memory {

/// Type of the reference counter attached to memory obtained from
/// \ref allocate_refcounted().
using refcount_type = unsigned;

/// Memory obtained from \ref allocate_refcounted(), and its reference counter.
struct refcounted_memory {
    /// The allocated memory, or \c nullptr if the allocation failed.
    void* memory = nullptr;
    /// Reference counter for \ref memory, or \c nullptr if the allocation
    /// failed.  It is initialized to 1.
    refcount_type* refcount = nullptr;
};

/// Allocates memory along with a reference counter for it.
///
/// This is like \c malloc(), except that the allocator also hands out space
/// for a reference counter, which can be used to manage the lifetime of the
/// memory without allocating a separate control block for it (see
/// \ref make_refcounted_deleter()).  The allocator picks a place for the
/// counter which is convenient for the allocation at hand: memory which owns
/// whole pages keeps it in allocator metadata, at no cost at all, while
/// smaller allocations get it stored next to the memory itself, at the cost of
/// a few bytes.
///
/// The memory is uninitialized, and is as aligned as memory returned by
/// \c malloc().  The reference counter is initialized to 1.
///
/// The counter is a plain integer, not an atomic one, so like \ref deleter,
/// all manipulations of it must be serialized.
///
/// \param size number of bytes to allocate
/// \return the memory and its reference counter, or a pair of null pointers if
///         the allocation failed
/// \related free_refcounted
refcounted_memory allocate_refcounted(size_t size) noexcept;

/// Frees memory obtained from \ref allocate_refcounted().
///
/// Takes back exactly what \ref allocate_refcounted() handed out.  Usually
/// called when the counter drops to zero.
///
/// \param memory memory and reference counter obtained from
///        \ref allocate_refcounted(); neither may be null
/// \related allocate_refcounted
void free_refcounted(refcounted_memory memory) noexcept;

/// Recovers memory obtained from \ref allocate_refcounted() from its reference
/// counter.
///
/// Intended for holders which have room for the reference counter only, such
/// as \ref deleter, and need the memory back in order to free it.
///
/// \param refcount reference counter obtained from \ref allocate_refcounted();
///        must not be null
/// \return the memory the counter refers to, and the counter itself
/// \related allocate_refcounted
refcounted_memory refcounted_memory_of(refcount_type* refcount) noexcept;

}

/// @}

}
