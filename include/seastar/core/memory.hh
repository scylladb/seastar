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

#pragma once

#include <seastar/core/resource.hh>
#include <seastar/core/bitops.hh>
#include <seastar/util/backtrace.hh>
#include <seastar/util/modules.hh>
#include <seastar/util/sampler.hh>
#ifndef SEASTAR_MODULE
#include <new>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#endif

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
/// You can obtain the memory layout of the current shard with
/// \ref get_memory_layout().
///
/// ## Critical allocation scopes
///
/// Seastar supports marking scopes as critical allocation scopes for the purpose
/// of special treatment from various memory related utilities.
/// See \ref scoped_critical_alloc_section.
///
/// ## Diagnostics and debugging features
///
/// ### Allocation failure injector
///
/// Allows injecting allocation failures for testing resiliency against
/// allocation failures, or exceptions in general. See:
/// * \ref alloc_failure_injector
/// * \ref with_allocation_failures()
///
/// ### Large allocation warning
///
/// Large allocations put great pressure on the allocator which might be unable
/// to serve them even if there is enough memory available, due to memory
/// fragmentation. This is especially relevant for long-running applications,
/// the kind of applications that are typically built with seastar. This feature
/// allows finding these large by logging a warning on large allocations, with
/// the stacktrace of the. See:
/// * \ref set_large_allocation_warning_threshold()
/// * \ref get_large_allocation_warning_threshold()
/// * \ref scoped_large_allocation_warning_threshold
/// * \ref scoped_large_allocation_warning_disable
///
/// ### Heap profiling
///
/// Heap profiling allows finding out how memory is used by your application, by
/// recording the stacktrace of a sampled subset (or all) allocations. See:
/// * \ref set_heap_profiling_sampling_rate()
/// * \ref sampled_memory_profile()
/// * \ref scoped_heap_profiling
///
/// ### Abort on allocation failure
///
/// Often, the best way to debug an allocation failure is a coredump. This
/// feature allows dumping core on allocation failures, containing the stack of
/// the failed allocation, by means of aborting. To enable this behavior, set
/// `abort_on_seastar_bad_alloc` in `reactor_options` or pass the
/// `--abort-on-seastar-bad-alloc` command line flag. Additionally, applications
/// may enable or disable this functionality globally at runtime by calling
/// `set_abort_on_allocation_failure()`.
///
/// ### Dump diagnostics report
///
/// Dump a diagnostic report of the state of the seastar allocator upon allocation
/// failure. The report is dumped with the `seastar_memory` logger, with debug
/// level.
/// You can configure a report to be dumped with error level on certain allocation
/// kinds, see:
/// * set_dump_memory_diagnostics_on_alloc_failure_kind()
/// * set_additional_diagnostics_producer()
/// * generate_memory_diagnostics_report()
///
/// The diagnostics report dump can be configured with the command
/// line/configuration file via the \p dump-memory-diagnostics-on-alloc-failure-kind
/// command-line flag/configuration item.
namespace memory {

/// \cond internal

#ifdef SEASTAR_OVERRIDE_ALLOCATOR_PAGE_SIZE
#define SEASTAR_INTERNAL_ALLOCATOR_PAGE_SIZE (SEASTAR_OVERRIDE_ALLOCATOR_PAGE_SIZE)
#else
#define SEASTAR_INTERNAL_ALLOCATOR_PAGE_SIZE 4096
#endif

constexpr inline size_t page_size = SEASTAR_INTERNAL_ALLOCATOR_PAGE_SIZE;
constexpr inline size_t page_bits = log2ceil(page_size);
constexpr inline size_t huge_page_size =
#if defined(__x86_64__) || defined(__i386__) || defined(__s390x__) || defined(__zarch__)
    1 << 21; // 2M
#elif defined(__aarch64__)
    1 << 21; // 2M
#elif defined(__PPC__)
    1 << 24; // 16M
#else
#error "Huge page size is not defined for this architecture"
#endif


namespace internal {

struct memory_range {
    char* start;
    char* end;
    unsigned numa_node_id;
};

struct numa_layout {
    std::vector<memory_range> ranges;
};

numa_layout merge(numa_layout one, numa_layout two);

}

internal::numa_layout configure(std::vector<resource::memory> m, bool mbind,
        bool transparent_hugepages,
        std::optional<std::string> hugetlbfs_path = {});

void configure_minimal();

// A deprecated alias for set_abort_on_allocation_failure(true).
[[deprecated("use set_abort_on_allocation_failure(true) instead")]]
void enable_abort_on_allocation_failure();

class disable_abort_on_alloc_failure_temporarily {
public:
    disable_abort_on_alloc_failure_temporarily();
    ~disable_abort_on_alloc_failure_temporarily() noexcept;
};

// Disables heap profiling as long as this object is alive.
// Can be nested, in which case the profiling is re-enabled when all
// the objects go out of scope.
class disable_backtrace_temporarily {
    sampler::disable_sampling_temporarily _disable_sampling;
public:
    disable_backtrace_temporarily();
};

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
    struct request {
        // The number of bytes which is needed to be released.
        // The reclaimer can release a different amount.
        // If less is released then the reclaimer may be invoked again.
        size_t bytes_to_reclaim;
    };
    using reclaim_fn = std::function<reclaiming_result ()>;
private:
    std::function<reclaiming_result (request)> _reclaim;
    reclaimer_scope _scope;
public:
    // Installs new reclaimer which will be invoked when system is falling
    // low on memory. 'scope' determines when reclaimer can be executed.
    reclaimer(std::function<reclaiming_result ()> reclaim, reclaimer_scope scope = reclaimer_scope::async);
    reclaimer(std::function<reclaiming_result (request)> reclaim, reclaimer_scope scope = reclaimer_scope::async);
    ~reclaimer();
    reclaiming_result do_reclaim(size_t bytes_to_reclaim) { return _reclaim(request{bytes_to_reclaim}); }
    reclaimer_scope scope() const { return _scope; }
};

extern std::pmr::polymorphic_allocator<char>* malloc_allocator;

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

/// \endcond

SEASTAR_MODULE_EXPORT_BEGIN

/// \brief Set the global state of the abort on allocation failure behavior.
///
/// If enabled, an allocation failure (i.e., the requested memory
/// could not be allocated even after reclaim was attempted), will
/// generally result in `abort()` being called. If disabled, the
/// failure is reported to the caller, e.g., by throwing a
/// `std::bad_alloc` for C++ allocations such as new, or returning
/// a null pointer from malloc.
///
/// Note that even if the global state is set to enabled, the
/// `disable_abort_on_alloc_failure_temporarily` class may override
/// the behavior tepmorarily on a given shard. That is, abort only
/// occurs if abort is globablly enabled on this shard _and_ there
/// are no `disable_abort_on_alloc_failure_temporarily` objects
/// currently alive on this shard.
void set_abort_on_allocation_failure(bool enabled);

/// \brief Determine the abort on allocation failure mode.
///
/// Return true if the global abort on allocation failure behavior
/// is enabled, or false otherwise. Always returns false if the
/// default (system) allocator is being used.
bool is_abort_on_allocation_failure();

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
    uint64_t _large_allocs;
    uint64_t _failed_allocs;

    uint64_t _foreign_mallocs;
    uint64_t _foreign_frees;
    uint64_t _foreign_cross_frees;
private:
    statistics(uint64_t mallocs, uint64_t frees, uint64_t cross_cpu_frees,
            uint64_t total_memory, uint64_t free_memory, uint64_t reclaims,
            uint64_t large_allocs, uint64_t failed_allocs,
            uint64_t foreign_mallocs, uint64_t foreign_frees, uint64_t foreign_cross_frees)
        : _mallocs(mallocs), _frees(frees), _cross_cpu_frees(cross_cpu_frees)
        , _total_memory(total_memory), _free_memory(free_memory), _reclaims(reclaims)
        , _large_allocs(large_allocs), _failed_allocs(failed_allocs)
        , _foreign_mallocs(foreign_mallocs), _foreign_frees(foreign_frees)
        , _foreign_cross_frees(foreign_cross_frees) {}
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
    /// Number of allocations which violated the large allocation threshold
    uint64_t large_allocations() const { return _large_allocs; }
    /// Number of allocations which failed, i.e., where the required memory could not be obtained
    /// even after reclaim was attempted
    uint64_t failed_allocations() const { return _failed_allocs; }
    /// Number of foreign allocations
    uint64_t foreign_mallocs() const { return _foreign_mallocs; }
    /// Number of foreign frees
    uint64_t foreign_frees() const { return _foreign_frees; }
    /// Number of foreign frees on reactor threads
    uint64_t foreign_cross_frees() const { return _foreign_cross_frees; }
    friend statistics stats();
};

struct memory_layout {
    uintptr_t start;
    uintptr_t end;
};

// Discover virtual address range used by the allocator on current shard.
// Supported only when seastar allocator is enabled.
memory::memory_layout get_memory_layout();

/// Returns the size of free memory in bytes.
size_t free_memory();

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

/// @brief Describes an allocation location in the code.
///
/// The location is identified by its backtrace. One allocation_site can
/// represent many allocations at the same location. `count` and `size`
/// represent the cumulative sum of all allocations at the location. Note the
/// size represents an extrapolated size and not the sampled one, i.e.: when
/// looking at the total size of all allocation sites it will approximate the
/// total memory usage
struct allocation_site {
    mutable size_t count = 0; /// number of live objects allocated at backtrace.
    mutable size_t size = 0; /// amount of bytes in live objects allocated at backtrace.
    simple_backtrace backtrace; /// call site for this allocation

    // All allocation sites are linked to each other. This can be used for easy
    // iteration across them in gdb scripts where it's difficult to work with
    // the C++ data structures.
    mutable const allocation_site* next = nullptr; // next allocation site in the chain
    mutable const allocation_site* prev = nullptr; // previous allocation site in the chain

    bool operator==(const allocation_site& o) const {
        return backtrace == o.backtrace;
    }

    bool operator!=(const allocation_site& o) const {
        return !(*this == o);
    }
};

/// @brief If memory sampling is on returns the current sampled memory live set
///
/// If there is tracked allocations (because heap profiling was on earlier)
/// these will still be returned if heap profiling is now off
///
/// @return a vector of \ref allocation_site
std::vector<allocation_site> sampled_memory_profile();

/// @brief Copies the current sampled set of allocation_sites into the
/// array pointed to by the output parameter
///
/// Copies up to \p size elements of the current sampled set of allocation
/// sites into the output array, which must have length at least \p size.
///
/// Returns amount of copied elements. This method does not allocate so it
/// is a useful alternative to \ref sampled_memory_profile() when one wants
/// to avoid allocating (e.g.: under OOM conditions).
///
/// @param output array to copy the allocation sites to
/// @param size the size of the array pointed to by \p output
/// @return number of \ref allocation_site copied to the vector
size_t sampled_memory_profile(allocation_site* output, size_t size);

/// @brief Enable sampled heap profiling by setting a sample rate
///
/// @param sample_rate the sample rate to use. Disable heap profiling by setting
/// the sample rate to 0
///
/// In order to use heap profiling you have to define
/// `SEASTAR_HEAPPROF`.
///
/// Use \ref sampled_memory_profile for API access to profiling data
///
/// Note: Changing the sampling rate while previously sampled allocations are
/// still alive can lead to inconsistent results of their reported size (i.e.:
/// their size will be over or under reported). Undefined behavior or memory
/// corruption will not occur.
///
/// For an example script that makes use of the heap profiling data
/// see [scylla-gdb.py] (https://github.com/scylladb/scylla/blob/e1b22b6a4c56b4f1d0adf65d1a11db4bcb51fe7d/scylla-gdb.py#L1439)
/// This script can generate either textual representation of the data,
/// or a zoomable flame graph ([flame graph generation instructions](https://github.com/scylladb/scylla/wiki/Seastar-heap-profiler),
/// [example flame graph](https://user-images.githubusercontent.com/1389273/72920437-f0cf8a80-3d51-11ea-92f0-f3dbeb698871.png)).
void set_heap_profiling_sampling_rate(size_t sample_rate);

/// @brief Returns the current heap profiling sampling rate (0 means off)
/// @return the current heap profiling sampling rate
size_t get_heap_profiling_sample_rate();

/// @brief Enable heap profiling for the duration of the scope.
///
/// Note: Nesting different sample rates is currently not supported.
///
/// For more information about heap profiling see
/// \ref set_heap_profiling_sampling_rate().
class scoped_heap_profiling {
public:
    scoped_heap_profiling(size_t) noexcept;
    ~scoped_heap_profiling();
};

SEASTAR_MODULE_EXPORT_END

}
}

namespace std {

template<>
struct hash<seastar::memory::allocation_site> {
    size_t operator()(const seastar::memory::allocation_site& bi) const {
        return std::hash<seastar::simple_backtrace>()(bi.backtrace);
    }
};

}
