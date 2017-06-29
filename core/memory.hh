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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#ifndef MEMORY_HH_
#define MEMORY_HH_

#include "resource.hh"
#include "bitops.hh"
#include <new>
#include <functional>
#include <vector>

namespace seastar {

/// \defgroup memory-module Memory management
///
/// Functions and classes for managing memory.
///
/// Memory management in seastar consists of the following:
///
///   - Low-level memory management in the \ref memory namespace.
///   - Various smart pointers: \ref shared_ptr, \ref lw_shared_ptr,
///     and \ref foreign_ptr.
///   - zero-copy support: \ref temporary_buffer and \ref deleter.

/// Low-level memory management support
///
/// The \c memory namespace provides functions and classes for interfacing
/// with the seastar memory allocator.
///
/// The seastar memory allocator splits system memory into a pool per
/// logical core (lcore).  Memory allocated one an lcore should be freed
/// on the same lcore; failing to do so carries a severe performance
/// penalty.  It is possible to share memory with another core, but this
/// should be limited to avoid cache coherency traffic.
namespace memory {

/// \cond internal

#ifdef SEASTAR_OVERRIDE_ALLOCATOR_PAGE_SIZE
#define SEASTAR_INTERNAL_ALLOCATOR_PAGE_SIZE (SEASTAR_OVERRIDE_ALLOCATOR_PAGE_SIZE)
#else
#define SEASTAR_INTERNAL_ALLOCATOR_PAGE_SIZE 4096
#endif

static constexpr size_t page_size = SEASTAR_INTERNAL_ALLOCATOR_PAGE_SIZE;
static constexpr size_t page_bits = log2ceil(page_size);
static constexpr size_t huge_page_size =
#if defined(__x86_64__) || defined(__i386__) || defined(__s390x__) || defined(__zarch__)
    1 << 21; // 2M
#elif defined(__PPC__)
    1 << 24; // 16M
#else
#error "Huge page size is not defined for this architecture"
#endif

void configure(std::vector<resource::memory> m, bool mbind,
        std::experimental::optional<std::string> hugetlbfs_path = {});

void enable_abort_on_allocation_failure();

class disable_abort_on_alloc_failure_temporarily {
public:
    disable_abort_on_alloc_failure_temporarily();
    ~disable_abort_on_alloc_failure_temporarily() noexcept;
};

void set_heap_profiling_enabled(bool);

enum class reclaiming_result {
    reclaimed_nothing,
    reclaimed_something
};

// Determines when reclaimer can be invoked
enum class reclaimer_scope {
    //
    // Reclaimer is only invoked in its own fiber. That fiber will be
    // given higher priority than regular application fibers.
    //
    async,

    //
    // Reclaimer may be invoked synchronously with allocation.
    // It may also be invoked in async scope.
    //
    // Reclaimer may invoke allocation, though it is discouraged because
    // the system may be low on memory and such allocations may fail.
    // Reclaimers which allocate should be prepared for re-entry.
    //
    sync
};

class reclaimer {
public:
    using reclaim_fn = std::function<reclaiming_result ()>;
private:
    reclaim_fn _reclaim;
    reclaimer_scope _scope;
public:
    // Installs new reclaimer which will be invoked when system is falling
    // low on memory. 'scope' determines when reclaimer can be executed.
    reclaimer(reclaim_fn reclaim, reclaimer_scope scope = reclaimer_scope::async);
    ~reclaimer();
    reclaiming_result do_reclaim() { return _reclaim(); }
    reclaimer_scope scope() const { return _scope; }
};

// Call periodically to recycle objects that were freed
// on cpu other than the one they were allocated on.
//
// Returns @true if any work was actually performed.
bool drain_cross_cpu_freelist();


// We don't want the memory code calling back into the rest of
// the system, so allow the rest of the system to tell the memory
// code how to initiate reclaim.
//
// When memory is low, calling \c hook(fn) will result in fn being called
// in a safe place wrt. allocations.
void set_reclaim_hook(
        std::function<void (std::function<void ()>)> hook);

using physical_address = uint64_t;

struct translation {
    translation() = default;
    translation(physical_address a, size_t s) : addr(a), size(s) {}
    physical_address addr = 0;
    size_t size = 0;
};

// Translate a virtual address range to a physical range.
//
// Can return a smaller range (in which case the reminder needs
// to be translated again), or a zero sized range in case the
// translation is not known.
translation translate(const void* addr, size_t size);

/// \endcond

class statistics;

/// Capture a snapshot of memory allocation statistics for this lcore.
statistics stats();

/// Memory allocation statistics.
class statistics {
    uint64_t _mallocs;
    uint64_t _frees;
    uint64_t _cross_cpu_frees;
    size_t _total_memory;
    size_t _free_memory;
    uint64_t _reclaims;
private:
    statistics(uint64_t mallocs, uint64_t frees, uint64_t cross_cpu_frees,
            uint64_t total_memory, uint64_t free_memory, uint64_t reclaims)
        : _mallocs(mallocs), _frees(frees), _cross_cpu_frees(cross_cpu_frees)
        , _total_memory(total_memory), _free_memory(free_memory), _reclaims(reclaims) {}
public:
    /// Total number of memory allocations calls since the system was started.
    uint64_t mallocs() const { return _mallocs; }
    /// Total number of memory deallocations calls since the system was started.
    uint64_t frees() const { return _frees; }
    /// Total number of memory deallocations that occured on a different lcore
    /// than the one on which they were allocated.
    uint64_t cross_cpu_frees() const { return _cross_cpu_frees; }
    /// Total number of objects which were allocated but not freed.
    size_t live_objects() const { return mallocs() - frees(); }
    /// Total free memory (in bytes)
    size_t free_memory() const { return _free_memory; }
    /// Total allocated memory (in bytes)
    size_t allocated_memory() const { return _total_memory - _free_memory; }
    /// Total memory (in bytes)
    size_t total_memory() const { return _total_memory; }
    /// Number of reclaims performed due to low memory
    uint64_t reclaims() const { return _reclaims; }
    friend statistics stats();
};

struct memory_layout {
    uintptr_t start;
    uintptr_t end;
};

// Discover virtual address range used by the allocator on current shard.
// Supported only when seastar allocator is enabled.
memory::memory_layout get_memory_layout();

/// Returns the value of free memory low water mark in bytes.
/// When free memory is below this value, reclaimers are invoked until it goes above again.
size_t min_free_memory();

/// Sets the value of free memory low water mark in memory::page_size units.
void set_min_free_pages(size_t pages);

/// Enable the large allocation warning threshold.
///
/// Warn when allocation above a given threshold are performed.
///
/// \param threshold size (in bytes) above which an allocation will be logged
void set_large_allocation_warning_threshold(size_t threshold);

/// Gets the current large allocation warning threshold.
size_t get_large_allocation_warning_threshold();

/// Disable large allocation warnings.
void disable_large_allocation_warning();

/// Set a different large allocation warning threshold for a scope.
class scoped_large_allocation_warning_threshold {
    size_t _old_threshold;
public:
    explicit scoped_large_allocation_warning_threshold(size_t threshold)
            : _old_threshold(get_large_allocation_warning_threshold()) {
        set_large_allocation_warning_threshold(threshold);
    }
    scoped_large_allocation_warning_threshold(const scoped_large_allocation_warning_threshold&) = delete;
    scoped_large_allocation_warning_threshold(scoped_large_allocation_warning_threshold&& x) = delete;
    ~scoped_large_allocation_warning_threshold() {
        if (_old_threshold) {
            set_large_allocation_warning_threshold(_old_threshold);
        }
    }
    void operator=(const scoped_large_allocation_warning_threshold&) const = delete;
    void operator=(scoped_large_allocation_warning_threshold&&) = delete;
};

/// Disable large allocation warnings for a scope.
class scoped_large_allocation_warning_disable {
    size_t _old_threshold;
public:
    scoped_large_allocation_warning_disable()
            : _old_threshold(get_large_allocation_warning_threshold()) {
        disable_large_allocation_warning();
    }
    scoped_large_allocation_warning_disable(const scoped_large_allocation_warning_disable&) = delete;
    scoped_large_allocation_warning_disable(scoped_large_allocation_warning_disable&& x) = delete;
    ~scoped_large_allocation_warning_disable() {
        if (_old_threshold) {
            set_large_allocation_warning_threshold(_old_threshold);
        }
    }
    void operator=(const scoped_large_allocation_warning_disable&) const = delete;
    void operator=(scoped_large_allocation_warning_disable&&) = delete;
};

}

class with_alignment {
    size_t _align;
public:
    with_alignment(size_t align) : _align(align) {}
    size_t alignment() const { return _align; }
};

}

void* operator new(size_t size, seastar::with_alignment wa);
void* operator new[](size_t size, seastar::with_alignment wa);
void operator delete(void* ptr, seastar::with_alignment wa);
void operator delete[](void* ptr, seastar::with_alignment wa);

#endif /* MEMORY_HH_ */
