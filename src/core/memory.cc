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


/// \cond internal

//
// Seastar memory allocator
//
// This is a share-nothing allocator (memory allocated on one cpu must
// be freed on the same cpu).
//
// Inspired by gperftools' tcmalloc.
//
// Memory map:
//
// 0x0000'sccc'vvvv'vvvv
//
// 0000 - required by architecture (only 48 bits of address space)
// s    - chosen to satisfy system allocator (1-7)
// ccc  - cpu number (0-12 bits allocated vary according to system)
// v    - virtual address within cpu (32-44 bits, according to how much ccc
//        leaves us
//
// Each page has a page structure that describes it.  Within a cpu's
// memory pool, the page array starts at offset 0, describing all pages
// within that pool.  Page 0 does not describe a valid page.
//
// Each pool can contain at most 2^32 pages (or 44 address bits), so we can
// use a 32-bit integer to identify a page.
//
// Runs of pages are organized into spans.  Free spans are organized into lists,
// by size.  When spans are broken up or coalesced, they may move into new lists.
// Spans have a size that is a power-of-two and are naturally aligned (aka buddy
// allocator)
//
// If compiled with SEASTAR_HEAPPROF seastar features a sampling memory
// profiler. Allocations are sampled at random (see `sampler` for the sampling
// logic) and tracked. The sampled live set can be retrieved with
// `sampled_memory_profile()`. Sampled allocations carry an extra
// allocation_site pointer with them which is used on free to remove them from
// the sampled live set.
//
// Large allocations are tracked via a pointer to the allocation_site which is
// stored on the page structure. To check whether an allocation was sampled or
// not this pointer is being looked at on free.
//
// Small allocations store an extra 8 bytes at the end of their allocation.
// Sampled allocations are allocated in a separate set of small pools. Hence, to
// check whether an allocation was sampled or not one only has to look at the
// tag in pool.
//

#ifdef SEASTAR_MODULE
module;
#endif

#include <concepts>
#include <unordered_set>
#include <iostream>
#include <optional>
#include <memory_resource>
#include <thread>

#include <seastar/util/assert.hh>

#include <fmt/format.h>
#include <fmt/ostream.h>

#include <boost/container/static_vector.hpp>

#include <dlfcn.h>

#ifndef SEASTAR_DEFAULT_ALLOCATOR
#include <new>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <atomic>
#include <mutex>
#include <functional>
#include <cstring>
#include <utility>
#include <boost/intrusive/list.hpp>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/mempolicy.h>

#endif // !defined(SEASTAR_DEFAULT_ALLOCATOR)

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/cacheline.hh>
#include <seastar/core/memory.hh>
#include <seastar/core/print.hh>
#include <seastar/util/alloc_failure_injector.hh>
#include <seastar/util/memory_diagnostics.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/sampler.hh>
#include <seastar/util/log.hh>
#include <seastar/core/aligned_buffer.hh>
#ifndef SEASTAR_DEFAULT_ALLOCATOR
#include <seastar/core/bitops.hh>
#include <seastar/core/align.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/util/backtrace.hh>
#endif
#endif

#ifdef SEASTAR_DEBUG
#define dassert(expr) SEASTAR_ASSERT(expr)
#else
#define dassert(expr) do {} while(false)
#endif

namespace seastar {

extern seastar::logger seastar_logger;

void* internal::allocate_aligned_buffer_impl(size_t size, size_t align) {
    void *ret;
    auto r = posix_memalign(&ret, align, size);
    if (r == ENOMEM) {
        throw std::bad_alloc();
    } else if (r == EINVAL) {
        throw std::runtime_error(format("Invalid alignment of {:d}; allocating {:d} bytes", align, size));
    } else {
        SEASTAR_ASSERT(r == 0);
        return ret;
    }
}

namespace memory {

// We always create the logger object for memory disagnostics, even in
// in SEASTAR_DEFAULT_ALLOCATOR builds, though it only logs when the
// seastar allocator is enabled.
seastar::logger seastar_memory_logger("seastar_memory");

static thread_local int abort_on_alloc_failure_suppressed = 0;

disable_abort_on_alloc_failure_temporarily::disable_abort_on_alloc_failure_temporarily() {
    ++abort_on_alloc_failure_suppressed;
}

disable_abort_on_alloc_failure_temporarily::~disable_abort_on_alloc_failure_temporarily() noexcept {
    --abort_on_alloc_failure_suppressed;
}

void enable_abort_on_allocation_failure() {
    set_abort_on_allocation_failure(true);
}

static std::pmr::polymorphic_allocator<char> static_malloc_allocator{std::pmr::get_default_resource()};;
std::pmr::polymorphic_allocator<char>* malloc_allocator{&static_malloc_allocator};

namespace internal {

#ifdef __cpp_constinit
#define SEASTAR_CONSTINIT constinit
#else
#define SEASTAR_CONSTINIT
#endif

#ifdef SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION

#ifdef __cpp_constinit
thread_local constinit volatile int critical_alloc_section = 0;
#else
__thread volatile int critical_alloc_section = 0;
#endif

#endif  // SEASTAR_ENABLE_ALLOC_FAILURE_INJECTION

numa_layout
merge(numa_layout one, numa_layout two) {
    // There's no chance to merge, so just concatenate
    one.ranges.insert(one.ranges.end(), two.ranges.begin(), two.ranges.end());
    return one;
}

} // namespace internal

}

}

#ifndef SEASTAR_DEFAULT_ALLOCATOR

#if FMT_VERSION >= 90000
namespace seastar::memory {
struct human_readable_value;
}
template <> struct fmt::formatter<struct seastar::memory::human_readable_value> : fmt::ostream_formatter {};
#endif

namespace seastar {

using allocation_site_ptr = const memory::allocation_site*;

namespace memory {

[[gnu::unused]]
static allocation_site_ptr get_allocation_site();

[[gnu::noinline]]
static void on_allocation_failure(size_t size);

static constexpr unsigned cpu_id_shift = 36; // FIXME: make dynamic
static constexpr unsigned max_cpus = 256;
static constexpr uintptr_t cpu_id_and_mem_base_mask = ~((uintptr_t(1) << cpu_id_shift) - 1);

using pageidx = uint32_t;

struct page;
class page_list;

static std::atomic<bool> live_cpus[max_cpus];

using std::optional;

// is_reactor_thread gets set to true when memory::configure() gets called
// it is used to identify seastar threads and hence use system memory allocator
// for those threads
static thread_local bool is_reactor_thread = false;

// We default transparent hugepages to true since we prefer to transiently
// use a transparent hugepage and then break it, to having the kernel
// work to rearrange a broken transparent hugepage.
std::atomic<bool> use_transparent_hugepages = true;

namespace alloc_stats {

enum class types { allocs, frees, cross_cpu_frees, reclaims, large_allocs, failed_allocs,
    foreign_mallocs, foreign_frees, foreign_cross_frees, enum_size };

using stats_array = std::array<uint64_t, static_cast<std::size_t>(types::enum_size)>;
using stats_atomic_array = std::array<std::atomic_uint64_t, static_cast<std::size_t>(types::enum_size)>;

static thread_local SEASTAR_CONSTINIT stats_array stats{};
std::array<stats_atomic_array, max_cpus> alien_stats{};

static void increment_local(types stat_type, uint64_t size = 1) {
    stats[static_cast<std::size_t>(stat_type)] += size;
}

static void increment(types stat_type, uint64_t size=1)
{
    // fast path, reactor threads takes thread local statistics
    if (is_reactor_thread) {
        increment_local(stat_type, size);
    } else {
        auto hash = std::hash<std::thread::id>()(std::this_thread::get_id());
        auto i = static_cast<std::size_t>(stat_type);
        alien_stats[hash % alien_stats.size()][i].fetch_add(size, std::memory_order_relaxed);
    }
}

static uint64_t get(types stat_type)
{
    auto i = static_cast<std::size_t>(stat_type);
    // fast path, reactor threads takes thread local statistics
    if (is_reactor_thread) {
        return stats[i];
    } else {
        auto hash = std::hash<std::thread::id>()(std::this_thread::get_id());
        return alien_stats[hash % alien_stats.size()][i].load();
    }
}

}

// original memory allocator support
// note: allocations before calling the constructor would use seastar allocator
using malloc_func_type = void * (*)(size_t);
using free_func_type = void * (*)(void *);
using realloc_func_type = void * (*)(void *, size_t);
using aligned_alloc_type = void * (*)(size_t alignment, size_t size);
using malloc_trim_type = int (*)(size_t);
using malloc_usable_size_type = size_t (*)(void *);

malloc_func_type original_malloc_func = reinterpret_cast<malloc_func_type>(dlsym(RTLD_NEXT, "malloc"));
free_func_type original_free_func = reinterpret_cast<free_func_type>(dlsym(RTLD_NEXT, "free"));
realloc_func_type original_realloc_func = reinterpret_cast<realloc_func_type>(dlsym(RTLD_NEXT, "realloc"));
aligned_alloc_type original_aligned_alloc_func = reinterpret_cast<aligned_alloc_type>(dlsym(RTLD_NEXT, "aligned_alloc"));
malloc_trim_type original_malloc_trim_func = reinterpret_cast<malloc_trim_type>(dlsym(RTLD_NEXT, "malloc_trim"));
malloc_usable_size_type original_malloc_usable_size_func = reinterpret_cast<malloc_usable_size_type>(dlsym(RTLD_NEXT, "malloc_usable_size"));

using allocate_system_memory_fn
        = std::function<mmap_area (void* where, size_t how_much)>;

namespace bi = boost::intrusive;

static thread_local uintptr_t local_expected_cpu_id = std::numeric_limits<uintptr_t>::max();

inline
unsigned object_cpu_id(const void* ptr) {
    return (reinterpret_cast<uintptr_t>(ptr) >> cpu_id_shift) & 0xff;
}

class page_list_link {
    uint32_t _prev;
    uint32_t _next;
    friend class page_list;
    friend seastar::internal::log_buf::inserter_iterator do_dump_memory_diagnostics(seastar::internal::log_buf::inserter_iterator);
};

constexpr size_t mem_base_alloc = size_t(1) << 44;

static char* mem_base() {
    static char* known;
    static std::once_flag flag;
    std::call_once(flag, [] {
        auto r = ::mmap(NULL, 2 * mem_base_alloc,
                    PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                    -1, 0);
        if (r == MAP_FAILED) {
            abort();
        }
        ::madvise(r, 2 * mem_base_alloc, MADV_DONTDUMP);
        auto cr = reinterpret_cast<char*>(r);
        known = align_up(cr, mem_base_alloc);
        ::munmap(cr, known - cr);
        ::munmap(known + mem_base_alloc, cr + 2 * mem_base_alloc - (known + mem_base_alloc));
        // extremely unlikely for mmap to return a mapping at 0, but our detection of free(null)
        // depends on it not doing that so check it
        SEASTAR_ASSERT(known != nullptr);
        SEASTAR_ASSERT(reinterpret_cast<uintptr_t>(known) != 0);
    });
    return known;
}

bool is_seastar_memory(void * ptr)
{
    auto begin = mem_base();
    auto end = begin + mem_base_alloc;
    return ptr >= begin && ptr < end;
}

constexpr bool is_page_aligned(size_t size) {
    return (size & (page_size - 1)) == 0;
}

constexpr size_t next_page_aligned(size_t size) {
    return (size + (page_size - 1)) & ~(page_size - 1);
}

class small_pool;

struct free_object {
    free_object* next;
};

struct page {
    bool free;
    uint8_t offset_in_span;
    uint16_t nr_small_alloc;
    uint32_t span_size; // in pages, if we're the head or the tail
    page_list_link link;
    small_pool* pool;  // if used in a small_pool
    free_object* freelist;
#ifdef SEASTAR_HEAPPROF
    allocation_site_ptr alloc_site; // for objects whose size is multiple of page size, valid for head only
#endif
};

class page_list {
    uint32_t _front = 0;
    uint32_t _back = 0;
public:
    page& front(page* ary) { return ary[_front]; }
    page& back(page* ary) { return ary[_back]; }
    bool empty() const { return !_front; }
    void erase(page* ary, page& span) {
        if (span.link._next) {
            ary[span.link._next].link._prev = span.link._prev;
        } else {
            _back = span.link._prev;
        }
        if (span.link._prev) {
            ary[span.link._prev].link._next = span.link._next;
        } else {
            _front = span.link._next;
        }
    }
    void push_front(page* ary, page& span) {
        auto idx = &span - ary;
        if (_front) {
            ary[_front].link._prev = idx;
        } else {
            _back = idx;
        }
        span.link._next = _front;
        span.link._prev = 0;
        _front = idx;
    }
    void pop_front(page* ary) {
        if (ary[_front].link._next) {
            ary[ary[_front].link._next].link._prev = 0;
        } else {
            _back = 0;
        }
        _front = ary[_front].link._next;
    }
    friend seastar::internal::log_buf::inserter_iterator do_dump_memory_diagnostics(seastar::internal::log_buf::inserter_iterator);
};

class small_pool {
    struct span_sizes {
        uint8_t preferred;
        uint8_t fallback;
    };
    free_object* _free = nullptr;
    unsigned _object_size;
    span_sizes _span_sizes;
    unsigned _free_count = 0;
    unsigned _min_free;
    unsigned _max_free;
    unsigned _pages_in_use = 0;
    // Flag to indicate whether this pool stores sampled allocations.
    // When freeing small allocations this flag is checked to see whether an
    // allocation site pointer is part of the object and the allocation needs
    // removal from the allocation_site tracking
#ifdef SEASTAR_HEAPPROF
    bool _sampled_pool = false;
#endif
    page_list _span_list;
    static constexpr unsigned idx_frac_bits = 2;
public:
    explicit small_pool(unsigned object_size, bool is_sampled) noexcept;
    ~small_pool();
    inline void* allocate();
    void deallocate(void* object);
    unsigned object_size() const { return _object_size; }
    /// See _sampled_pool
    bool is_sampled_pool() const {
#ifdef SEASTAR_HEAPPROF
        return _sampled_pool;
#else
        return false;
#endif
    }
    bool objects_page_aligned() const { return is_page_aligned(_object_size); }
    static constexpr unsigned size_to_idx(unsigned size);
    static constexpr unsigned idx_to_size(unsigned idx);
    allocation_site_ptr& alloc_site_holder(void* ptr);
private:
    inline void* pop_free();
    [[gnu::noinline]] void* add_more_objects();
    void trim_free_list();
    friend seastar::internal::log_buf::inserter_iterator do_dump_memory_diagnostics(seastar::internal::log_buf::inserter_iterator);
};

// index 0b0001'1100 -> size (1 << 4) + 0b11 << (4 - 2)

constexpr unsigned
small_pool::idx_to_size(unsigned idx) {
    size_t s = (((1 << idx_frac_bits) | (idx & ((1 << idx_frac_bits) - 1)))
              << (idx >> idx_frac_bits))
                  >> idx_frac_bits;
    // If size is larger than max_align_t, force it to be a multiple of
    // max_align_t. Clang relies in this property to use aligned mov
    // instructions (e.g. movaps)
    //
    // Note this function is used at initialization time only, so it doesn't
    // need to be especially fast.
    if (s > alignof(std::max_align_t)) {
	s = align_up(s, alignof(std::max_align_t));
    }
    return s;
}

constexpr unsigned
small_pool::size_to_idx(unsigned size) {
    return ((log2floor(size) << idx_frac_bits) - ((1 << idx_frac_bits) - 1))
            + ((size - 1) >> (log2floor(size) - idx_frac_bits));
}

template<bool sampled> // tag the pools in this array as sampled, see small_pool._sampled_pool
class small_pool_array {
public:
    static constexpr unsigned nr_small_pools = small_pool::size_to_idx(4 * page_size) + 1;
private:
    union u {
        small_pool a[nr_small_pools];
        u() {
            for (unsigned i = 0; i < nr_small_pools; ++i) {
                new (&a[i]) small_pool(small_pool::idx_to_size(i), sampled);
            }
        }
        ~u() {
            // cannot really call destructor, since other
            // objects may be freed after we are gone.
        }
    } _u;
public:
    small_pool& operator[](unsigned idx) { return _u.a[idx]; }
};

static constexpr size_t max_small_allocation
    = small_pool::idx_to_size(small_pool_array<false>::nr_small_pools - 1);

#ifdef SEASTAR_HEAPPROF
constexpr size_t object_size_with_alloc_site(size_t size) {
    // For page-aligned sizes, allocation_site* lives in page::alloc_site, not with the object.
    static_assert(is_page_aligned(max_small_allocation), "assuming that max_small_allocation is page aligned so that we"
            " don't need to add allocation_site_ptr to objects of size close to it");
    size_t next_page_aligned_size = next_page_aligned(size);
    if (next_page_aligned_size - size > sizeof(allocation_site_ptr)) {
        size += sizeof(allocation_site_ptr);
    } else {
        return next_page_aligned_size;
    }
    return size;
}

// Ensure that object_size_with_alloc_site() does not exceed max_small_allocation
static_assert(object_size_with_alloc_site(max_small_allocation) == max_small_allocation, "");
static_assert(object_size_with_alloc_site(max_small_allocation - 1) == max_small_allocation, "");
static_assert(object_size_with_alloc_site(max_small_allocation - sizeof(allocation_site_ptr) + 1) == max_small_allocation, "");
static_assert(object_size_with_alloc_site(max_small_allocation - sizeof(allocation_site_ptr)) == max_small_allocation, "");
static_assert(object_size_with_alloc_site(max_small_allocation - sizeof(allocation_site_ptr) - 1) == max_small_allocation - 1, "");
static_assert(object_size_with_alloc_site(max_small_allocation - sizeof(allocation_site_ptr) - 2) == max_small_allocation - 2, "");
#endif

struct cross_cpu_free_item {
    cross_cpu_free_item* next;
};

struct cpu_pages {
    small_pool_array<false> small_pools;
    uint32_t min_free_pages = 20000000 / page_size;
    char* memory;
    page* pages;
    uint32_t nr_pages;
    uint32_t nr_free_pages;
    uint32_t current_min_free_pages = 0;
    size_t large_allocation_warning_threshold = std::numeric_limits<size_t>::max();
    unsigned cpu_id = -1U;
    std::function<void (std::function<void ()>)> reclaim_hook;
    std::vector<reclaimer*> reclaimers;
    static constexpr unsigned nr_span_lists = 32;
    page_list free_spans[nr_span_lists];  // contains aligned spans with span_size == 2^idx
    alignas(seastar::cache_line_size) std::atomic<cross_cpu_free_item*> xcpu_freelist;
    static std::atomic<unsigned> cpu_id_gen;
    static cpu_pages* all_cpus[max_cpus];
    union asu {
        using alloc_sites_type = std::unordered_set<allocation_site>;
        asu() : alloc_sites{} {
        }
        ~asu() {} // alloc_sites live forever
        alloc_sites_type alloc_sites;
    } asu;
    allocation_site_ptr alloc_site_list_head = nullptr; // For easy traversal of asu.alloc_sites from scylla-gdb.py
    sampler heap_prof_sampler;
    small_pool_array<true> sampled_small_pools;

    char* mem() { return memory; }

    void link(page_list& list, page* span);
    void unlink(page_list& list, page* span);
    struct trim {
        unsigned offset;
        unsigned nr_pages;
    };
    void maybe_reclaim();
    void* allocate_large_and_trim(unsigned nr_pages, bool should_sample);
    void* allocate_large(unsigned nr_pages, bool should_sample);
    void* allocate_large_aligned(unsigned align_pages, unsigned nr_pages, bool should_sample);
    page* find_and_unlink_span(unsigned nr_pages);
    page* find_and_unlink_span_reclaiming(unsigned n_pages);
    void free_large(void* ptr);
    bool grow_span(pageidx& start, uint32_t& nr_pages, unsigned idx);
    void free_span(pageidx start, uint32_t nr_pages);
    void free_span_no_merge(pageidx start, uint32_t nr_pages);
    void free_span_unaligned(pageidx start, uint32_t nr_pages);
    void free(void* ptr);
    void free(void* ptr, size_t size);
    static bool try_free_fastpath(void* ptr);
    static bool is_local_pointer(void* ptr);
    static void do_foreign_free(void* ptr);
    void shrink(void* ptr, size_t new_size);
    static void free_cross_cpu(unsigned cpu_id, void* ptr);
    bool drain_cross_cpu_freelist();
    size_t object_size(void* ptr);

    page* to_page(void* p) {
        size_t page_idx = ((uintptr_t)p) << (64 - cpu_id_shift) >> (64 - cpu_id_shift + page_bits);
        return &pages[page_idx];
    }

    bool is_initialized() const;
    bool initialize();
    reclaiming_result run_reclaimers(reclaimer_scope, size_t pages_to_reclaim);
    void schedule_reclaim();
    void set_reclaim_hook(std::function<void (std::function<void ()>)> hook);
    void set_min_free_pages(size_t pages);
    void resize(size_t new_size, allocate_system_memory_fn alloc_sys_mem);
    void do_resize(size_t new_size, allocate_system_memory_fn alloc_sys_mem);
    void replace_memory_backing(allocate_system_memory_fn alloc_sys_mem);
    void check_large_allocation(size_t size);
    void warn_large_allocation(size_t size);
    allocation_site_ptr add_alloc_site(size_t allocated_size);
    void remove_alloc_site(allocation_site_ptr alloc_site, size_t deallocated_size);
    bool maybe_sample(size_t size);
    bool definitely_sample(size_t size);
    memory::memory_layout memory_layout();
    ~cpu_pages();
};

static thread_local cpu_pages cpu_mem;
std::atomic<unsigned> cpu_pages::cpu_id_gen;
cpu_pages* cpu_pages::all_cpus[max_cpus];

static cpu_pages& get_cpu_mem();

#ifdef SEASTAR_HEAPPROF

void set_heap_profiling_sampling_rate(size_t sample_rate) {
    bool current_sample_rate = get_cpu_mem().heap_prof_sampler.sampling_interval();
    if (sample_rate) {
        if (!current_sample_rate) {
            seastar_logger.info("Enabling heap profiler - using {} bytes sampling rate", sample_rate);
        } else {
            seastar_logger.warn("Ignoring change to heap profiler sample rate as heap profiling is already turned on");
            return;
        }
    } else {
        if (current_sample_rate) {
            seastar_logger.info("Disabling heap profiler");
        }
    }
    get_cpu_mem().heap_prof_sampler.set_sampling_interval(sample_rate);
}

size_t get_heap_profiling_sample_rate() {
    return get_cpu_mem().heap_prof_sampler.sampling_interval();
}

static thread_local int64_t scoped_heap_profiling_embed_count = 0;

scoped_heap_profiling::scoped_heap_profiling(size_t sample_rate) noexcept {
    ++scoped_heap_profiling_embed_count;
    set_heap_profiling_sampling_rate(sample_rate);
}

scoped_heap_profiling::~scoped_heap_profiling() {
    if (!--scoped_heap_profiling_embed_count) {
        set_heap_profiling_sampling_rate(0);
    }
}

#else

void set_heap_profiling_sampling_rate(size_t enable) {
    seastar_logger.warn("Seastar compiled without heap profiling support, heap profiler not supported;"
            " compile with the Seastar_HEAP_PROFILING=ON CMake option to add heap profiling support");
}

size_t get_heap_profiling_sample_rate() {
    // don't log here, called on all paths
    return 0;
}

scoped_heap_profiling::scoped_heap_profiling(size_t sample_rate) noexcept {
    set_heap_profiling_sampling_rate(sample_rate); // let it print the warning
}

scoped_heap_profiling::~scoped_heap_profiling() {
}

#endif

// Smallest index i such that all spans stored in the index are >= pages.
static inline
unsigned index_of(unsigned pages) {
    if (pages == 1) {
        return 0;
    }
    return std::numeric_limits<unsigned>::digits - count_leading_zeros(pages - 1);
}

void
cpu_pages::unlink(page_list& list, page* span) {
    list.erase(pages, *span);
}

void
cpu_pages::link(page_list& list, page* span) {
    list.push_front(pages, *span);
}

void cpu_pages::free_span_no_merge(uint32_t span_start, uint32_t nr_pages) {
    SEASTAR_ASSERT(nr_pages);
    nr_free_pages += nr_pages;
    auto span = &pages[span_start];
    auto span_end = &pages[span_start + nr_pages - 1];
    span->free = span_end->free = true;
    span->span_size = span_end->span_size = nr_pages;
    auto idx = index_of(nr_pages);
    link(free_spans[idx], span);
}

bool cpu_pages::grow_span(uint32_t& span_start, uint32_t& nr_pages, unsigned idx) {
    auto which = (span_start >> idx) & 1; // 0=lower, 1=upper
    // locate first page of upper buddy or last page of lower buddy
    // examples: span_start = 0x10 nr_pages = 0x08 -> buddy = 0x18  (which = 0)
    //           span_start = 0x18 nr_pages = 0x08 -> buddy = 0x17  (which = 1)
    // delta = which ? -1u : nr_pages
    auto delta = ((which ^ 1) << idx) | -which;
    auto buddy = span_start + delta;
    if (pages[buddy].free && pages[buddy].span_size == nr_pages) {
        unlink(free_spans[idx], &pages[span_start ^ nr_pages]);
        nr_free_pages -= nr_pages; // free_span_no_merge() will restore
        span_start &= ~nr_pages;
        nr_pages *= 2;
        return true;
    }
    return false;
}

void cpu_pages::free_span(uint32_t span_start, uint32_t nr_pages) {
    auto idx = index_of(nr_pages);
    while (grow_span(span_start, nr_pages, idx)) {
        ++idx;
    }
    free_span_no_merge(span_start, nr_pages);
}

// Internal, used during startup. Span is not aligned so needs to be broken up
void cpu_pages::free_span_unaligned(uint32_t span_start, uint32_t nr_pages) {
    while (nr_pages) {
        auto start_nr_bits = span_start ? count_trailing_zeros(span_start) : 32;
        auto size_nr_bits = count_trailing_zeros(nr_pages);
        auto now = 1u << std::min(start_nr_bits, size_nr_bits);
        free_span(span_start, now);
        span_start += now;
        nr_pages -= now;
    }
}

page*
cpu_pages::find_and_unlink_span(unsigned n_pages) {
    auto idx = index_of(n_pages);
    if (n_pages >= (2u << idx)) {
        return nullptr;
    }
    while (idx < nr_span_lists && free_spans[idx].empty()) {
        ++idx;
    }
    if (idx == nr_span_lists) {
        if (initialize()) {
            return find_and_unlink_span(n_pages);
        }
        return nullptr;
    }
    auto& list = free_spans[idx];
    page* span = &list.front(pages);
    unlink(list, span);
    return span;
}

page*
cpu_pages::find_and_unlink_span_reclaiming(unsigned n_pages) {
    while (true) {
        auto span = find_and_unlink_span(n_pages);
        if (span) {
            return span;
        }
        if (run_reclaimers(reclaimer_scope::sync, n_pages) == reclaiming_result::reclaimed_nothing) {
            return nullptr;
        }
    }
}

void cpu_pages::maybe_reclaim() {
    if (nr_free_pages < current_min_free_pages) {
        drain_cross_cpu_freelist();
        if (nr_free_pages < current_min_free_pages) {
            run_reclaimers(reclaimer_scope::sync, current_min_free_pages - nr_free_pages);
        }
        if (nr_free_pages < current_min_free_pages) {
            schedule_reclaim();
        }
    }
}

void*
cpu_pages::allocate_large_and_trim(unsigned n_pages, bool should_sample) {
    // Avoid exercising the reclaimers for requests we'll not be able to satisfy
    // nr_pages might be zero during startup, so check for that too
    if (nr_pages && n_pages >= nr_pages) {
        return nullptr;
    }
    page* span = find_and_unlink_span_reclaiming(n_pages);
    if (!span) {
        return nullptr;
    }
    auto span_size = span->span_size;
    auto span_idx = span - pages;
    nr_free_pages -= span->span_size;
    while (span_size >= n_pages * 2) {
        span_size /= 2;
        auto other_span_idx = span_idx + span_size;
        free_span_no_merge(other_span_idx, span_size);
    }
    auto span_end = &pages[span_idx + span_size - 1];
    span->free = span_end->free = false;
    span->span_size = span_end->span_size = span_size;
    span->pool = nullptr;
#ifdef SEASTAR_HEAPPROF
    if (should_sample) {
        auto alloc_site = add_alloc_site(span->span_size * page_size);
        span->alloc_site = alloc_site;
    }
    else {
        span->alloc_site = nullptr;
    }
#endif
    maybe_reclaim();
    return mem() + span_idx * page_size;
}

void
cpu_pages::warn_large_allocation(size_t size) {
    alloc_stats::increment_local(alloc_stats::types::large_allocs);
    seastar_memory_logger.warn("oversized allocation: {} bytes. This is non-fatal, but could lead to latency and/or fragmentation issues. Please report: at {}", size, current_backtrace());
    large_allocation_warning_threshold *= 1.618; // prevent spam
}

allocation_site_ptr
cpu_pages::add_alloc_site(size_t allocated_size) {
    allocation_site_ptr alloc_site = get_allocation_site();
    if (alloc_site) {
        ++alloc_site->count;
        alloc_site->size += heap_prof_sampler.sample_size(allocated_size);
    }

    return alloc_site;
}

void
cpu_pages::remove_alloc_site(allocation_site_ptr alloc_site, size_t deallocated_size) {
    if (alloc_site) {
        --alloc_site->count;
        auto sample_size = heap_prof_sampler.sample_size(deallocated_size);
        // prevent underflow in case sample rate changed
        alloc_site->size -= alloc_site->size < sample_size ? alloc_site->size : sample_size;
        if (alloc_site->count == 0) {
            if (alloc_site->prev) {
                alloc_site->prev->next = alloc_site->next;
            }
            if (alloc_site->next) {
                alloc_site->next->prev = alloc_site->prev;
            }
            if (alloc_site_list_head == alloc_site) {
                alloc_site_list_head = alloc_site->next;
            }

            asu.alloc_sites.erase(*alloc_site);
        }
    }
}

[[gnu::always_inline]]
inline bool
cpu_pages::maybe_sample(size_t size) {
#ifdef SEASTAR_HEAPPROF
    return heap_prof_sampler.maybe_sample(size);
#else
    return false;
#endif
}

[[gnu::always_inline]]
inline bool
cpu_pages::definitely_sample(size_t size) {
#ifdef SEASTAR_HEAPPROF
    return heap_prof_sampler.definitely_sample(size);
#else
    return false;
#endif
}

[[gnu::always_inline]]
void
inline
cpu_pages::check_large_allocation(size_t size) {
    if (size >= large_allocation_warning_threshold) {
        warn_large_allocation(size);
    }
}

[[gnu::always_inline]]
inline void*
cpu_pages::allocate_large(unsigned n_pages, bool should_sample) {
    check_large_allocation(n_pages * page_size);
    return allocate_large_and_trim(n_pages, should_sample);
}

void*
cpu_pages::allocate_large_aligned(unsigned align_pages, unsigned n_pages, bool should_sample) {
    check_large_allocation(n_pages * page_size);
    // buddy allocation is always aligned
    return allocate_large_and_trim(n_pages, should_sample);
}

disable_backtrace_temporarily::disable_backtrace_temporarily()
    : _disable_sampling(cpu_mem.heap_prof_sampler.pause_sampling()) {
}

static
simple_backtrace get_backtrace() noexcept {
    disable_backtrace_temporarily dbt;
    return current_backtrace_tasklocal();
}

static
allocation_site_ptr get_allocation_site() {
    if (!cpu_mem.is_initialized() || !cpu_mem.heap_prof_sampler.sampling_interval()) {
        return nullptr;
    }
    disable_backtrace_temporarily dbt;
    allocation_site new_alloc_site;
    new_alloc_site.backtrace = get_backtrace();
    if (cpu_mem.asu.alloc_sites.size() >= 1000
        && cpu_mem.asu.alloc_sites.find(new_alloc_site) == cpu_mem.asu.alloc_sites.end()) {
        // Drop sample for now. Could do something smarter like dropping a
        // current one at random but needs more work in remove_alloc_site as we
        // might then have allocations for which the allocsite is no longer
        // alive
        return nullptr;
    }
    auto insert_result = cpu_mem.asu.alloc_sites.insert(std::move(new_alloc_site));
    allocation_site_ptr alloc_site = &*insert_result.first;
    if (insert_result.second) {
        alloc_site->next = cpu_mem.alloc_site_list_head;
        if (cpu_mem.alloc_site_list_head) {
            cpu_mem.alloc_site_list_head->prev = alloc_site;
        }
        cpu_mem.alloc_site_list_head = alloc_site;
    }
    return alloc_site;
}

#ifdef SEASTAR_HEAPPROF

allocation_site_ptr&
small_pool::alloc_site_holder(void* ptr) {
    if (objects_page_aligned()) {
        return get_cpu_mem().to_page(ptr)->alloc_site;
    } else {
        return *reinterpret_cast<allocation_site_ptr*>(reinterpret_cast<char*>(ptr) + _object_size - sizeof(allocation_site_ptr));
    }
}

#endif

static
void
maybe_enable_transparent_hugepages(void* addr, size_t len) {
    if (use_transparent_hugepages.load(std::memory_order_relaxed)) {
        ::madvise(addr, len, MADV_HUGEPAGE);
    }
}

static
void
maybe_disable_transparent_hugepages(void* addr, size_t len) {
    if (!use_transparent_hugepages.load(std::memory_order_relaxed)) {
        ::madvise(addr, len, MADV_NOHUGEPAGE);
    }
}

void cpu_pages::free_large(void* ptr) {
    pageidx idx = (reinterpret_cast<char*>(ptr) - mem()) / page_size;
    page* span = &pages[idx];
#ifdef SEASTAR_HEAPPROF
    if (span->alloc_site) {
        auto alloc_site = span->alloc_site;
        remove_alloc_site(alloc_site, span->span_size * page_size);
    }
#endif
    free_span(idx, span->span_size);
}

size_t cpu_pages::object_size(void* ptr) {
    page* span = to_page(ptr);
    if (span->pool) {
        auto s = span->pool->object_size();
#ifdef SEASTAR_HEAPPROF
        if (span->pool->is_sampled_pool()) {
            // We must not allow the object to be extended onto the allocation_site_ptr field.
            if (!span->pool->objects_page_aligned()) {
                s -= sizeof(allocation_site_ptr);
            }
        }
#endif
        return s;
    } else {
        return size_t(span->span_size) * page_size;
    }
}

void cpu_pages::free_cross_cpu(unsigned cpu_id, void* ptr) {
    if (!live_cpus[cpu_id].load(std::memory_order_relaxed)) {
        // Thread was destroyed; leak object
        // should only happen for boost unit-tests.
        return;
    }
    auto p = reinterpret_cast<cross_cpu_free_item*>(ptr);
    auto& list = all_cpus[cpu_id]->xcpu_freelist;
    auto old = list.load(std::memory_order_relaxed);
    do {
        p->next = old;
    } while (!list.compare_exchange_weak(old, p, std::memory_order_release, std::memory_order_relaxed));
    alloc_stats::increment(alloc_stats::types::cross_cpu_frees);
}

bool cpu_pages::drain_cross_cpu_freelist() {
    if (!xcpu_freelist.load(std::memory_order_relaxed)) {
        return false;
    }
    auto p = xcpu_freelist.exchange(nullptr, std::memory_order_acquire);
    while (p) {
        auto n = p->next;
        alloc_stats::increment_local(alloc_stats::types::frees);
        free(p);
        p = n;
    }
    return true;
}

[[gnu::always_inline]]
inline void cpu_pages::free(void* ptr) {
    page* span = to_page(ptr);
    if (span->pool) {
        small_pool& pool = *span->pool;
#ifdef SEASTAR_HEAPPROF
        if (pool.is_sampled_pool()) {
            allocation_site_ptr alloc_site = pool.alloc_site_holder(ptr);
            remove_alloc_site(alloc_site, pool.object_size());
        }
#endif
        pool.deallocate(ptr);
    } else {
        free_large(ptr);
    }
}

void cpu_pages::free(void* ptr, size_t size) {
#ifdef SEASTAR_HEAPPROF
    // sized free can avoid accessing the `page` structure as an optimization.
    // With memory sampling we always need to check the pool though to see
    // whether this allocation was sampled. Hence, just defer to the non-sized
    // implementation
    (void) size;
    free(ptr);
#else
    // match action on allocate() so hit the right pool
    if (size <= sizeof(free_object)) {
        size = sizeof(free_object);
    }
    if (size <= max_small_allocation) {
        auto pool = &small_pools[small_pool::size_to_idx(size)];
        pool->deallocate(ptr);
    } else {
        free_large(ptr);
    }
#endif
}

// Is the passed pointer a local pointer, i.e., allocated on the current shard from the
// per-shard allocator.
[[gnu::always_inline]]
inline bool
cpu_pages::is_local_pointer(void* ptr) {
    return (reinterpret_cast<uintptr_t>(ptr) & cpu_id_and_mem_base_mask) == local_expected_cpu_id;
}

// Try to execute free on the fast path, which succeeds if:
//
// 1) The pointer is local to this shard
// 2) The pointer is from a small pool
// 3) The small pool is not sampled
//
// In this case, complete the de-allocation and return true.
// Otherwise, modify nothing and return false.
[[gnu::always_inline]]
inline bool
cpu_pages::try_free_fastpath(void* ptr) {
    if (__builtin_expect(is_local_pointer(ptr), true)) {
        auto pool = get_cpu_mem().to_page(ptr)->pool;
        if (__builtin_expect(pool && !pool->is_sampled_pool(), true)) {
            alloc_stats::increment_local(alloc_stats::types::frees);
            pool->deallocate(ptr);
            return true;
        }
    }
    return false;
}

/// Helper to allow a single implementation for sized and non-sized functions.
/// Indicator to allow a single implementation for sized and non-sized functions.
/// The size parameter will be either no_size tag type or size_t, and most
/// of the implementation can be shared, using constexpr if or other dispatch
/// in the places where there should be a difference of behavior.
struct no_size {};

template <typename S>
requires std::same_as<S, size_t> || std::same_as<S, no_size>
[[gnu::noinline]]
static void free_slowpath(void* obj, S size) {
    if (cpu_pages::is_local_pointer(obj)) {
        alloc_stats::increment_local(alloc_stats::types::frees);
        if constexpr (std::is_same_v<decltype(size), no_size>) {
            get_cpu_mem().free(obj);
        } else {
            get_cpu_mem().free(obj, size);
        }
    } else {
        cpu_pages::do_foreign_free(obj);
    }
}

[[gnu::noinline]]
void
cpu_pages::do_foreign_free(void* ptr) {
    // handles:
    // 1) non-seastar pointers
    // 2) cross-shard frees
    // 3) null pointer

    if (!ptr) {
        return;
    }

    if (!is_seastar_memory(ptr)) {
        if (is_reactor_thread) {
            alloc_stats::increment_local(alloc_stats::types::foreign_cross_frees);
        } else {
            alloc_stats::increment(alloc_stats::types::foreign_frees);
        }
        original_free_func(ptr);
        return;
    }
    free_cross_cpu(object_cpu_id(ptr), ptr);
}

void cpu_pages::shrink(void* ptr, size_t new_size) {
    SEASTAR_ASSERT(object_cpu_id(ptr) == cpu_id);
    page* span = to_page(ptr);
    if (span->pool) {
        return;
    }
    auto old_size_pages = span->span_size;
    size_t new_size_pages = old_size_pages;
    while (new_size_pages / 2 * page_size >= new_size) {
        new_size_pages /= 2;
    }
    if (new_size_pages == old_size_pages) {
        return;
    }
#ifdef SEASTAR_HEAPPROF
    auto alloc_site = span->alloc_site;
    if (alloc_site) {
        alloc_site->size -= span->span_size * page_size;
        alloc_site->size += new_size_pages * page_size;
    }
#endif
    span->span_size = new_size_pages;
    span[new_size_pages - 1].free = false;
    span[new_size_pages - 1].span_size = new_size_pages;
    pageidx idx = span - pages;
    free_span_unaligned(idx + new_size_pages, old_size_pages - new_size_pages);
}

cpu_pages::~cpu_pages() {
    if (is_initialized()) {
        live_cpus[cpu_id].store(false, std::memory_order_relaxed);
    }
}

bool cpu_pages::is_initialized() const {
    return bool(nr_pages);
}

bool cpu_pages::initialize() {
    if (is_initialized()) {
        return false;
    }
    cpu_id = cpu_id_gen.fetch_add(1, std::memory_order_relaxed);
    local_expected_cpu_id = (static_cast<uint64_t>(cpu_id) << cpu_id_shift)
	        | reinterpret_cast<uintptr_t>(mem_base());
    SEASTAR_ASSERT(cpu_id < max_cpus);
    all_cpus[cpu_id] = this;
    auto base = mem_base() + (size_t(cpu_id) << cpu_id_shift);
    auto size = 32 << 20;  // Small size for bootstrap
    auto r = ::mmap(base, size,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
            -1, 0);
    if (r == MAP_FAILED) {
        abort();
    }
    maybe_enable_transparent_hugepages(base, size);
    pages = reinterpret_cast<page*>(base);
    memory = base;
    nr_pages = size / page_size;
    // we reserve the end page so we don't have to special case
    // the last span.
    auto reserved = align_up(sizeof(page) * (nr_pages + 1), page_size) / page_size;
    reserved = 1u << log2ceil(reserved);
    for (pageidx i = 0; i < reserved; ++i) {
        pages[i].free = false;
    }
    pages[nr_pages].free = false;
    free_span_unaligned(reserved, nr_pages - reserved);
    live_cpus[cpu_id].store(true, std::memory_order_relaxed);
    return true;
}

mmap_area
static allocate_anonymous_memory(void* where, size_t how_much) {
    return mmap_anonymous(where,
            how_much,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_FIXED);
}

mmap_area
allocate_hugetlbfs_memory(file_desc& fd, void* where, size_t how_much) {
    auto pos = fd.size();
    fd.truncate(pos + how_much);
    auto ret = fd.map(
            how_much,
            PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE | (where ? MAP_FIXED : 0),
            pos,
            where);
    return ret;
}

void cpu_pages::replace_memory_backing(allocate_system_memory_fn alloc_sys_mem) {
    // We would like to use ::mremap() to atomically replace the old anonymous
    // memory with hugetlbfs backed memory, but mremap() does not support hugetlbfs
    // (for no reason at all).  So we must copy the anonymous memory to some other
    // place, map hugetlbfs in place, and copy it back, without modifying it during
    // the operation.
    auto bytes = nr_pages * page_size;
    auto old_mem = mem();
    auto relocated_old_mem = mmap_anonymous(nullptr, bytes, PROT_READ|PROT_WRITE, MAP_PRIVATE);
    std::memcpy(relocated_old_mem.get(), old_mem, bytes);
    alloc_sys_mem(old_mem, bytes).release();
    std::memcpy(old_mem, relocated_old_mem.get(), bytes);
}

void cpu_pages::do_resize(size_t new_size, allocate_system_memory_fn alloc_sys_mem) {
    auto new_pages = new_size / page_size;
    if (new_pages <= nr_pages) {
        return;
    }
    auto old_size = nr_pages * page_size;
    auto mmap_start = memory + old_size;
    auto mmap_size = new_size - old_size;
    auto mem = alloc_sys_mem(mmap_start, mmap_size);
    mem.release();
    maybe_enable_transparent_hugepages(mmap_start, mmap_size);
    // one past last page structure is a sentinel
    auto new_page_array_pages = align_up(sizeof(page) * (new_pages + 1), page_size) / page_size;
    auto new_page_array
        = reinterpret_cast<page*>(allocate_large(new_page_array_pages, false));
    if (!new_page_array) {
        throw std::bad_alloc();
    }
    std::copy(pages, pages + nr_pages, new_page_array);
    // mark new one-past-last page as taken to avoid boundary conditions
    new_page_array[new_pages].free = false;
    auto old_pages = reinterpret_cast<char*>(pages);
    auto old_nr_pages = nr_pages;
    auto old_pages_size = align_up(sizeof(page) * (nr_pages + 1), page_size);
    old_pages_size = size_t(1) << log2ceil(old_pages_size);
    pages = new_page_array;
    nr_pages = new_pages;
    auto old_pages_start = (old_pages - memory) / page_size;
    if (old_pages_start == 0) {
        // keep page 0 allocated
        old_pages_start = 1;
        old_pages_size -= page_size;
    }
    if (old_pages_size != 0) {
        free_span_unaligned(old_pages_start, old_pages_size / page_size);
    }
    free_span_unaligned(old_nr_pages, new_pages - old_nr_pages);
}

void cpu_pages::resize(size_t new_size, allocate_system_memory_fn alloc_memory) {
    new_size = align_down(new_size, huge_page_size);
    while (nr_pages * page_size < new_size) {
        // don't reallocate all at once, since there might not
        // be enough free memory available to relocate the pages array
        auto tmp_size = std::min(new_size, 4 * nr_pages * page_size);
        do_resize(tmp_size, alloc_memory);
    }
}

reclaiming_result cpu_pages::run_reclaimers(reclaimer_scope scope, size_t n_pages) {
    auto target = std::max<size_t>(nr_free_pages + n_pages, min_free_pages);
    reclaiming_result result = reclaiming_result::reclaimed_nothing;
    while (nr_free_pages < target) {
        bool made_progress = false;
        alloc_stats::increment_local(alloc_stats::types::reclaims);
        for (auto&& r : reclaimers) {
            if (r->scope() >= scope) {
                made_progress |= r->do_reclaim((target - nr_free_pages) * page_size) == reclaiming_result::reclaimed_something;
            }
        }
        if (!made_progress) {
            return result;
        }
        result = reclaiming_result::reclaimed_something;
    }
    return result;
}

void cpu_pages::schedule_reclaim() {
    current_min_free_pages = 0;
    reclaim_hook([this] {
        if (nr_free_pages < min_free_pages) {
            try {
                run_reclaimers(reclaimer_scope::async, min_free_pages - nr_free_pages);
            } catch (...) {
                current_min_free_pages = min_free_pages;
                throw;
            }
        }
        current_min_free_pages = min_free_pages;
    });
}

memory::memory_layout cpu_pages::memory_layout() {
    SEASTAR_ASSERT(is_initialized());
    return {
        reinterpret_cast<uintptr_t>(memory),
        reinterpret_cast<uintptr_t>(memory) + nr_pages * page_size
    };
}

void cpu_pages::set_reclaim_hook(std::function<void (std::function<void ()>)> hook) {
    reclaim_hook = hook;
    current_min_free_pages = min_free_pages;
}

void cpu_pages::set_min_free_pages(size_t pages) {
    if (pages > std::numeric_limits<decltype(min_free_pages)>::max()) {
        throw std::runtime_error("Number of pages too large");
    }
    min_free_pages = pages;
    maybe_reclaim();
}

small_pool::small_pool(unsigned object_size, bool is_sampled) noexcept
    : _object_size(object_size)
#ifdef SEASTAR_HEAPPROF
    , _sampled_pool(is_sampled)
#endif
    {
    unsigned span_size = 1;
    auto span_bytes = [&] { return span_size * page_size; };
    auto waste = [&] { return (span_bytes() % _object_size) / (1.0 * span_bytes()); };
    while (object_size > span_bytes()) {
        ++span_size;
    }
    _span_sizes.fallback = span_size;

    // Choose a preferred span size which keeps waste (internal fragmentation) below
    // 5% and fits at least 4 objects. If there is no span size (up to 32 pages) that
    // satisfies this, just go with the minimum waste out of the checked span sizes.
    float min_waste = std::numeric_limits<float>::max();
    unsigned min_waste_span_size = 0;
    for (span_size = 1; span_size <= 32; span_size *= 2) {
        if (span_bytes() / object_size >= 4) {
            auto w = waste();
            if (w < min_waste) {
                min_waste = w;
                min_waste_span_size = span_size;
                if (w < 0.05) {
                    break;
                }
            }
        }
    }
    _span_sizes.preferred = min_waste_span_size ? min_waste_span_size : _span_sizes.fallback;

    _max_free = std::max<unsigned>(100, span_bytes() * 2 / _object_size);
    _min_free = _max_free / 2;
    _free = nullptr;
}

small_pool::~small_pool() {
    _min_free = _max_free = 0;
    trim_free_list();
}

/**
 * Remove the next object from the freelist and return it.
 * It must exist (caller must check) or UB.
 */
void *
small_pool::pop_free() {
    auto* obj = _free;
    _free = _free->next;
    --_free_count;
    return obj;
}

// Should not throw in case of running out of memory to avoid infinite recursion,
// becaue throwing std::bad_alloc requires allocation. __cxa_allocate_exception
// falls back to the emergency pool in case malloc() returns nullptr.
void*
small_pool::allocate() {
    return __builtin_expect((bool)_free, true) ? pop_free() : add_more_objects();
}

void
small_pool::deallocate(void* object) {
    auto o = reinterpret_cast<free_object*>(object);
    o->next = _free;
    _free = o;
    ++_free_count;
    if (_free_count >= _max_free) {
        trim_free_list();
    }
}

void*
small_pool::add_more_objects() {
    auto goal = (_min_free + _max_free) / 2;
    while (!_span_list.empty() && _free_count < goal) {
        page& span = _span_list.front(get_cpu_mem().pages);
        _span_list.pop_front(get_cpu_mem().pages);
        while (span.freelist) {
            auto obj = span.freelist;
            span.freelist = span.freelist->next;
            obj->next = _free;
            _free = obj;
            ++_free_count;
            ++span.nr_small_alloc;
        }
    }
    while (_free_count < goal) {
        auto span_size = _span_sizes.preferred;
        auto data = reinterpret_cast<char*>(get_cpu_mem().allocate_large(span_size, false));
        if (!data) {
            span_size = _span_sizes.fallback;
            data = reinterpret_cast<char*>(get_cpu_mem().allocate_large(span_size, false));
            if (!data) {
                break;
            }
        }
        auto span = get_cpu_mem().to_page(data);
        span_size = span->span_size;
        _pages_in_use += span_size;
        for (unsigned i = 0; i < span_size; ++i) {
            span[i].offset_in_span = i;
            span[i].pool = this;
        }
        span->nr_small_alloc = 0;
        span->freelist = nullptr;
        for (size_t offset = 0; offset <= span_size * page_size - _object_size; offset += _object_size) {
            auto h = reinterpret_cast<free_object*>(data + offset);
            h->next = _free;
            _free = h;
            ++_free_count;
            ++span->nr_small_alloc;
        }
    }

    return _free ? pop_free() : nullptr;
}

void
small_pool::trim_free_list() {
    auto goal = (_min_free + _max_free) / 2;
    while (_free && _free_count > goal) {
        auto obj = _free;
        _free = _free->next;
        --_free_count;
        page* span = get_cpu_mem().to_page(obj);
        span -= span->offset_in_span;
        if (!span->freelist) {
            new (&span->link) page_list_link();
            _span_list.push_front(get_cpu_mem().pages, *span);
        }
        obj->next = span->freelist;
        span->freelist = obj;
        if (--span->nr_small_alloc == 0) {
            _pages_in_use -= span->span_size;
            _span_list.erase(get_cpu_mem().pages, *span);
            get_cpu_mem().free_span(span - get_cpu_mem().pages, span->span_size);
        }
    }
}

void
abort_on_underflow(size_t size) {
    if (std::make_signed_t<size_t>(size) < 0) {
        // probably a logic error, stop hard
        abort();
    }
}

[[gnu::always_inline]]
inline void* allocate_large(size_t size, bool should_sample) {
    abort_on_underflow(size);
    unsigned size_in_pages = (size + page_size - 1) >> page_bits;
    if ((size_t(size_in_pages) << page_bits) < size) {
        return nullptr; // (size + page_size - 1) caused an overflow
    }
    return get_cpu_mem().allocate_large(size_in_pages, should_sample);

}

void* allocate_large_aligned(size_t align, size_t size, bool should_sample) {
    abort_on_underflow(size);
    unsigned size_in_pages = (size + page_size - 1) >> page_bits;
    unsigned align_in_pages = std::max(align, page_size) >> page_bits;
    return get_cpu_mem().allocate_large_aligned(align_in_pages, size_in_pages, should_sample);
}

void free_large(void* ptr) {
    return get_cpu_mem().free_large(ptr);
}

size_t object_size(void* ptr) {
    return cpu_pages::all_cpus[object_cpu_id(ptr)]->object_size(ptr);
}

static thread_local cpu_pages* cpu_mem_ptr = nullptr;

// Mark as cold so that GCC8+ can move to .text.unlikely.
[[gnu::cold]]
static void init_cpu_mem() {
    cpu_mem_ptr = &cpu_mem;
    cpu_mem.initialize();
}

[[gnu::always_inline]]
static inline cpu_pages& get_cpu_mem()
{
    // cpu_pages has a non-trivial constructor which means that the compiler
    // must make sure the instance local to the current thread has been
    // constructed before each access. So instead we access cpu_mem_ptr
    // which has been initialized by calls to init_cpu_mem() before it is
    // accessed.
    return *cpu_mem_ptr;
}

#ifdef SEASTAR_DEBUG_ALLOCATIONS
static constexpr int debug_allocation_pattern = 0xab;
#endif

enum class alignment_t { aligned, unaligned };

#ifdef SEASTAR_HEAPPROF
template<alignment_t alignment>
void* allocate_from_sampled_small_pool(size_t size) {
    size = object_size_with_alloc_site(size);
    if constexpr (alignment == alignment_t::aligned) {
        size = 1 << log2ceil(size);
    }
    auto idx = small_pool::size_to_idx(size);
    auto& pool = get_cpu_mem().sampled_small_pools[idx];
    dassert(size <= pool.object_size());
    void* ptr = pool.allocate();
    if (__builtin_expect(ptr != nullptr, true)) {
        // we failed to allocate, so we won't sample either
        auto alloc_site = get_cpu_mem().add_alloc_site(pool.object_size());
        new (&pool.alloc_site_holder(ptr)) allocation_site_ptr{alloc_site};
    }
    return ptr;
}

#endif

template<alignment_t alignment>
void* allocate_from_small_pool(size_t size)
{
    if constexpr (alignment == alignment_t::aligned) {
        size = 1 << log2ceil(size);
    }
    auto idx = small_pool::size_to_idx(size);
    auto& pool = get_cpu_mem().small_pools[idx];
    dassert(size <= pool.object_size());
    return pool.allocate();
}

/**
 * Common handling code when a pointer (possibly null) has
 * been returned by any allocation path.
 */
[[gnu::always_inline]]
static inline void* finish_allocation(void* ptr, size_t size) {
    alloc_stats::increment_local(alloc_stats::types::allocs);
    if (__builtin_expect(!ptr, false)) {
        on_allocation_failure(size);
    } else {
#ifdef SEASTAR_DEBUG_ALLOCATIONS
        std::memset(ptr, debug_allocation_pattern, size);
#endif
    }
    return ptr;
}

void *allocate_slowpath(size_t size) {
    if (!is_reactor_thread) {
        if (original_malloc_func) {
            alloc_stats::increment(alloc_stats::types::foreign_mallocs);
            return original_malloc_func(size);
        }

        // original_malloc_func might be null for allocations before main
        // in constructors before original_malloc_func ctor is called
        // Note on #2137: Moved to here, because there is lots of code
        // that implicitly relies on the static init fiasco below to have occurred, and thus
        // cpu_mem_ptr being available and inited. This is not great.
        init_cpu_mem();

        // #2137 - static init fiasco for fallback functions.
        // If dependent libraries do malloc _before_ the above declaration inits are run,
        // we end up here with nowhere to go. Add a second check and attempt the full init
        // already. If we find the functions, all is good. Otherwise, trudge along and probably
        // crash terribly later.
        // Note: this is only relevant if we're in dlinit phase, in which case we are single
        // threaded. Or there is no external func to find. No need for atomics or fences.
        static bool double_checked = false;
        if (!double_checked) {
            double_checked = true;

            original_malloc_func = reinterpret_cast<malloc_func_type>(dlsym(RTLD_NEXT, "malloc"));
            original_free_func = reinterpret_cast<free_func_type>(dlsym(RTLD_NEXT, "free"));
            original_realloc_func = reinterpret_cast<realloc_func_type>(dlsym(RTLD_NEXT, "realloc"));
            original_aligned_alloc_func = reinterpret_cast<aligned_alloc_type>(dlsym(RTLD_NEXT, "aligned_alloc"));
            original_malloc_trim_func = reinterpret_cast<malloc_trim_type>(dlsym(RTLD_NEXT, "malloc_trim"));
            original_malloc_usable_size_func = reinterpret_cast<malloc_usable_size_type>(dlsym(RTLD_NEXT, "malloc_usable_size"));

            if (original_malloc_func) {
                return allocate_slowpath(size);
            }
        }
    }
    // On the fast path we've already called maybe_sample, except in the case
    // of !is_reactor_thread (we don't sample such alloctions).
    bool should_sample = get_cpu_mem().definitely_sample(size);
    void* ptr;
    if (size <= max_small_allocation) {
#ifdef SEASTAR_HEAPPROF
        if (should_sample) {
            ptr = allocate_from_sampled_small_pool<alignment_t::unaligned>(size);
        } else
#endif
        {
            ptr = allocate_from_small_pool<alignment_t::unaligned>(size);
        }
    } else {
        ptr = allocate_large(size, should_sample);
    }
    return finish_allocation(ptr, size);
}

/// The main entry point for allocation.
///
/// The idea is that function is inlined into malloc and all the variations of new
/// so that the body of each of those functions will contain the full fast path
/// fully inlined, and all of these variations will share the allocate_slowpath
/// fallback, which is not inlined.
[[gnu::always_inline]]
inline void* allocate(size_t size) {
    size = std::max(size, sizeof(free_object));
    if (__builtin_expect(is_reactor_thread && !get_cpu_mem().maybe_sample(size) && size <= max_small_allocation, true)) {
        auto ptr = allocate_from_small_pool<alignment_t::unaligned>(size);
        return finish_allocation(ptr, size);
    }

    return allocate_slowpath(size);
}

void* allocate_aligned(size_t align, size_t size) {
    if (!is_reactor_thread) {
        if (original_aligned_alloc_func) {
            alloc_stats::increment(alloc_stats::types::foreign_mallocs);
            return original_aligned_alloc_func(align, size);
        }
        // original_realloc_func might be null for allocations before main
        // in constructors before original_realloc_func ctor is called
        init_cpu_mem();
    }
    if (size <= sizeof(free_object)) {
        size = std::max(sizeof(free_object), align);
    }
#ifdef SEASTAR_HEAPPROF
    auto& mem = get_cpu_mem();
    bool should_sample = mem.maybe_sample(size) && mem.definitely_sample(size);
#else
    bool should_sample = false;
#endif
    void* ptr;
    if (size <= max_small_allocation && align <= page_size) {
        // Our small allocator only guarantees alignment for power-of-two
        // allocations which are not larger than a page.
#ifdef SEASTAR_HEAPPROF
        if (should_sample) {
            ptr = allocate_from_sampled_small_pool<alignment_t::aligned>(size);
        } else
#endif
        {
            ptr = allocate_from_small_pool<alignment_t::aligned>(size);
        }
    } else {
        ptr = allocate_large_aligned(align, size, should_sample);
    }
    if (!ptr) {
        on_allocation_failure(size);
    } else {
#ifdef SEASTAR_DEBUG_ALLOCATIONS
        std::memset(ptr, debug_allocation_pattern, size);
#endif
    }
    alloc_stats::increment_local(alloc_stats::types::allocs);
    return ptr;
}


/// Similarly to memory::allocate() we expect to inline the whole fast path
/// of free into all the variations of the top level dallocation functions like
/// free, delete, sized delete, etc. The slow path is not inline and is shared
/// by all implementations.
///
/// The S template object allows us to handle both sized and unsized allocations
/// in the same code path.
template <typename S = no_size>
[[gnu::always_inline]]
inline void free(void* obj, S size = {}) {
    if (!__builtin_expect(cpu_pages::try_free_fastpath(obj), true)) {
        free_slowpath(obj, size);
    }
}

void free_aligned(void* obj, size_t align, size_t size) {
    if (size <= sizeof(free_object)) {
        size = sizeof(free_object);
    }
#ifndef SEASTAR_HEAPPROF
    // See comment in the sized-free implementation below. With heap profiling we
    // can skip this as sized-free defers to non-sized-free
    if (size <= max_small_allocation && align <= page_size) {
        // Same adjustment as allocate_aligned()
        size = 1 << log2ceil(size);
    }
#endif
    free(obj, size);
}

void shrink(void* obj, size_t new_size) {
    alloc_stats::increment_local(alloc_stats::types::frees);
    alloc_stats::increment_local(alloc_stats::types::allocs); // keep them balanced
    get_cpu_mem().shrink(obj, new_size);
}

void set_reclaim_hook(std::function<void (std::function<void ()>)> hook) {
    // in general, we are using Seastar allocator here. but the Seastar application
    // can still configure smp_opts.memory_allocator with memory_allocator::standard.
    // in that case, the memory::configure() is not called, hence cpu_mem_ptr is not
    // set.
    if (cpu_mem_ptr) {
        cpu_mem_ptr->set_reclaim_hook(hook);
    }
}

reclaimer::reclaimer(std::function<reclaiming_result ()> reclaim, reclaimer_scope scope)
    : reclaimer([reclaim = std::move(reclaim)] (request) {
        return reclaim();
    }, scope) {
}

reclaimer::reclaimer(std::function<reclaiming_result (request)> reclaim, reclaimer_scope scope)
    : _reclaim(std::move(reclaim))
    , _scope(scope) {
    get_cpu_mem().reclaimers.push_back(this);
}

reclaimer::~reclaimer() {
    auto& r = get_cpu_mem().reclaimers;
    r.erase(std::find(r.begin(), r.end(), this));
}

void set_large_allocation_warning_threshold(size_t threshold) {
    get_cpu_mem().large_allocation_warning_threshold = threshold;
}

size_t get_large_allocation_warning_threshold() {
    return get_cpu_mem().large_allocation_warning_threshold;
}

void disable_large_allocation_warning() {
    get_cpu_mem().large_allocation_warning_threshold = std::numeric_limits<size_t>::max();
}

void configure_minimal() {
    init_cpu_mem();
}

static long mbind(void *addr,
                  unsigned long len,
                  int mode,
                  const unsigned long *nodemask,
                  unsigned long maxnode,
                  unsigned flags) {
    return syscall(
        SYS_mbind,
        addr,
        len,
        mode,
        nodemask,
        maxnode,
        flags
    );
}

internal::numa_layout
configure(std::vector<resource::memory> m, bool mbind,
        bool transparent_hugepages,
        optional<std::string> hugetlbfs_path) {
    // we need to make sure cpu_mem is initialize since configure calls cpu_mem.resize
    // and we might reach configure without ever allocating, hence without ever calling
    // cpu_pages::initialize.
    // The correct solution is to add a condition inside cpu_mem.resize, but since all
    // other paths to cpu_pages::resize are already verifying initialize was called, we
    // verify that here.
    use_transparent_hugepages.store(transparent_hugepages, std::memory_order_relaxed);
    init_cpu_mem();
    // init_cpu_mem() could have been called very early, see call site in allocate().
    // In that case we don't know about the transparent_hugepages parameter and conservatively
    // assume it's true. Undo that here if it turned out to be false.
    maybe_disable_transparent_hugepages(cpu_mem.memory, cpu_mem.nr_pages * page_size);
    is_reactor_thread = true;
    internal::numa_layout ret_layout;
    size_t total = 0;
    for (auto&& x : m) {
        total += x.bytes;
    }
    allocate_system_memory_fn sys_alloc = allocate_anonymous_memory;
    if (hugetlbfs_path) {
        // std::function is copyable, but file_desc is not, so we must use
        // a shared_ptr to allow sys_alloc to be copied around
        auto fdp = make_lw_shared<file_desc>(file_desc::temporary(*hugetlbfs_path));
        sys_alloc = [fdp] (void* where, size_t how_much) {
            return allocate_hugetlbfs_memory(*fdp, where, how_much);
        };
        get_cpu_mem().replace_memory_backing(sys_alloc);
    }
    get_cpu_mem().resize(total, sys_alloc);
    size_t pos = 0;
    for (auto&& x : m) {
        unsigned long nodemask = 1UL << x.nodeid;
        if (mbind) {
            auto start = get_cpu_mem().mem() + pos;
            auto r = seastar::memory::mbind(
                            start, x.bytes,
                            MPOL_PREFERRED,
                            &nodemask, std::numeric_limits<unsigned long>::digits,
                            MPOL_MF_MOVE);

            if (r == -1) {
                char err[1000] = {};
#ifdef SEASTAR_STRERROR_R_CHAR_P
                const char *msg = strerror_r(errno, err, sizeof(err));
#else
                const char *msg = strerror_r(errno, err, sizeof(err)) ? "unknown error" : err;
#endif
                std::cerr << "WARNING: unable to mbind shard memory; performance may suffer: "
                        << msg << std::endl;
            }
            ret_layout.ranges.push_back({.start = start, .end = start + x.bytes, .numa_node_id = x.nodeid});
        }
        pos += x.bytes;
    }
    return ret_layout;
}

statistics stats() {
    return statistics{alloc_stats::get(alloc_stats::types::allocs), alloc_stats::get(alloc_stats::types::frees), alloc_stats::get(alloc_stats::types::cross_cpu_frees),
        cpu_mem.nr_pages * page_size, cpu_mem.nr_free_pages * page_size, alloc_stats::get(alloc_stats::types::reclaims), alloc_stats::get(alloc_stats::types::large_allocs),
        alloc_stats::get(alloc_stats::types::failed_allocs), alloc_stats::get(alloc_stats::types::foreign_mallocs), alloc_stats::get(alloc_stats::types::foreign_frees),
        alloc_stats::get(alloc_stats::types::foreign_cross_frees)};
}

size_t free_memory() {
    return get_cpu_mem().nr_free_pages * page_size;
}

bool drain_cross_cpu_freelist() {
    return get_cpu_mem().drain_cross_cpu_freelist();
}

memory_layout get_memory_layout() {
    return get_cpu_mem().memory_layout();
}

size_t min_free_memory() {
    return get_cpu_mem().min_free_pages * page_size;
}

void set_min_free_pages(size_t pages) {
    get_cpu_mem().set_min_free_pages(pages);
}

static thread_local int report_on_alloc_failure_suppressed = 0;

class disable_report_on_alloc_failure_temporarily {
public:
    disable_report_on_alloc_failure_temporarily() {
        ++report_on_alloc_failure_suppressed;
    };
    ~disable_report_on_alloc_failure_temporarily() noexcept {
        --report_on_alloc_failure_suppressed;
    }
};

static std::atomic<bool> abort_on_allocation_failure{false};
static std::atomic<alloc_failure_kind> dump_diagnostics_on_alloc_failure_kind{alloc_failure_kind::critical};

void set_abort_on_allocation_failure(bool enabled) {
    abort_on_allocation_failure.store(enabled, std::memory_order_seq_cst);
}

bool is_abort_on_allocation_failure() {
    return abort_on_allocation_failure;
}

void set_dump_memory_diagnostics_on_alloc_failure_kind(alloc_failure_kind kind) {
    dump_diagnostics_on_alloc_failure_kind.store(kind, std::memory_order_seq_cst);
}

void set_dump_memory_diagnostics_on_alloc_failure_kind(std::string_view str) {
    if (str == "none") {
        set_dump_memory_diagnostics_on_alloc_failure_kind(alloc_failure_kind::none);
    } else if (str == "critical") {
        set_dump_memory_diagnostics_on_alloc_failure_kind(alloc_failure_kind::critical);
    } else if (str == "all") {
        set_dump_memory_diagnostics_on_alloc_failure_kind(alloc_failure_kind::all);
    } else {
        seastar_logger.error("Ignoring invalid option '{}' for the allocation failure kind to dump seastar memory diagnostics for, valid options are: none, critical and all", str);
    }
}

static thread_local noncopyable_function<void(memory_diagnostics_writer)> additional_diagnostics_producer;

void set_additional_diagnostics_producer(noncopyable_function<void(memory_diagnostics_writer)> producer) {
    additional_diagnostics_producer = std::move(producer);
}

struct human_readable_value {
    uint16_t value;  // [0, 1024)
    char suffix; // 0 -> no suffix
};

std::ostream& operator<<(std::ostream& os, const human_readable_value& val) {
    os << val.value;
    if (val.suffix) {
        os << val.suffix;
    }
    return os;
}

static human_readable_value to_human_readable_value(uint64_t value, uint64_t step, uint64_t precision, const std::array<char, 5>& suffixes) {
    if (!value) {
        return {0, suffixes[0]};
    }

    uint64_t result = value;
    uint64_t remainder = 0;
    unsigned i = 0;
    // If there is no remainder we go below precision because we don't loose any.
    while (((!remainder && result >= step) || result >= precision)) {
        remainder = result % step;
        result /= step;
        if (i == suffixes.size()) {
            break;
        } else {
            ++i;
        }
    }
    return {uint16_t(remainder < (step / 2) ? result : result + 1), suffixes[i]};
}

static human_readable_value to_hr_size(uint64_t size) {
    const std::array<char, 5> suffixes = {'B', 'K', 'M', 'G', 'T'};
    return to_human_readable_value(size, 1024, 8192, suffixes);
}

static human_readable_value to_hr_number(uint64_t number) {
    const std::array<char, 5> suffixes = {'\0', 'k', 'm', 'b', 't'};
    return to_human_readable_value(number, 1000, 10000, suffixes);
}

seastar::internal::log_buf::inserter_iterator do_dump_memory_diagnostics(seastar::internal::log_buf::inserter_iterator it) {
    auto free_mem = get_cpu_mem().nr_free_pages * page_size;
    auto total_mem = get_cpu_mem().nr_pages * page_size;
    it = fmt::format_to(it, "Dumping seastar memory diagnostics\n");

    it = fmt::format_to(it, "Used memory:   {}\n", to_hr_size(total_mem - free_mem));
    it = fmt::format_to(it, "Free memory:   {}\n", to_hr_size(free_mem));
    it = fmt::format_to(it, "Total memory:  {}\n", to_hr_size(total_mem));
    it = fmt::format_to(it, "Hard failures: {}\n\n", alloc_stats::get(alloc_stats::types::failed_allocs));

    if (additional_diagnostics_producer) {
        additional_diagnostics_producer([&it] (std::string_view v) mutable {
            it = fmt::format_to(it, fmt::runtime(v));
        });
    }

    it = fmt::format_to(it, "Small pools:\n");
    it = fmt::format_to(it, "objsz spansz usedobj memory unused wst%\n");
    for (unsigned i = 0; i < get_cpu_mem().small_pools.nr_small_pools; i++) {
        auto& sp = get_cpu_mem().small_pools[i];
        // We don't use pools too small to fit a free_object, so skip these, they
        // are always empty.
        if (sp.object_size() < sizeof(free_object)) {
            continue;
        }

        // For the small pools, there are two types of free objects:
        // Pool freelist objects are poitned to by sp._free and their count is sp._free_count
        // Span freelist objects are those removed from the pool freelist when that list
        // becomes too large: they are instead attached to the spans allocated to this
        // pool. To count this second category, we iterate over the spans below.
        uint32_t span_freelist_objs = 0;
        auto front = sp._span_list._front;
        while (front) {
            auto& span = get_cpu_mem().pages[front];
            auto capacity_in_objects = span.span_size * page_size / sp.object_size();
            span_freelist_objs += capacity_in_objects - span.nr_small_alloc;
            front = span.link._next;
        }
        const auto free_objs = sp._free_count + span_freelist_objs; // pool + span free objects
        const auto use_count = sp._pages_in_use * page_size / sp.object_size() - free_objs;
        auto memory = sp._pages_in_use * page_size;
        const auto unused = free_objs * sp.object_size();
        const auto wasted_percent = memory ? unused * 100 / memory : 0;
        it = fmt::format_to(it,
                "{:>5}  {:>5}   {:>5}  {:>5}  {:>5} {:>4}\n",
                sp.object_size(),
                to_hr_size(sp._span_sizes.preferred * page_size),
                to_hr_number(use_count),
                to_hr_size(memory),
                to_hr_size(unused),
                unsigned(wasted_percent));
    }
    it = fmt::format_to(it, "\nPage spans:\n");
    it = fmt::format_to(it, "index  size  free  used spans\n");

    std::array<uint32_t, cpu_pages::nr_span_lists> span_size_histogram;
    span_size_histogram.fill(0);

    for (unsigned i = 0; i < get_cpu_mem().nr_pages;) {
        const auto span_size = get_cpu_mem().pages[i].span_size;
        if (!span_size) {
            ++i;
            continue;
        }
        ++span_size_histogram[log2ceil(span_size)];
        i += span_size;
    }

    for (unsigned i = 0; i< get_cpu_mem().nr_span_lists; i++) {
        auto& span_list = get_cpu_mem().free_spans[i];
        auto front = span_list._front;
        uint32_t free_pages = 0;
        while (front) {
            auto& span = get_cpu_mem().pages[front];
            free_pages += span.span_size;
            front = span.link._next;
        }
        const auto total_spans = span_size_histogram[i];
        const auto total_pages = total_spans * (1 << i);
        it = fmt::format_to(it,
                "{:>5} {:>5} {:>5} {:>5} {:>5}\n",
                i,
                to_hr_size((uint64_t(1) << i) * page_size),
                to_hr_size(free_pages * page_size),
                to_hr_size((total_pages - free_pages) * page_size),
                to_hr_number(total_spans));
    }

    return it;
}

void dump_memory_diagnostics(log_level lvl, logger::rate_limit& rate_limit) {
    logger::lambda_log_writer writer([] (seastar::internal::log_buf::inserter_iterator it) {
        return do_dump_memory_diagnostics(it);
    });
    seastar_memory_logger.log(lvl, rate_limit, writer);
}

namespace internal {
void log_memory_diagnostics_report(log_level lvl) {
    logger::rate_limit rl{std::chrono::seconds(0)}; // never limit for explicit dump requests
    dump_memory_diagnostics(lvl, rl);
}
}

void maybe_dump_memory_diagnostics(size_t size, bool is_aborting) {
    if (report_on_alloc_failure_suppressed) {
        return;
    }

    disable_report_on_alloc_failure_temporarily guard;
    if (seastar_memory_logger.is_enabled(log_level::debug)) {
        seastar_memory_logger.debug("Failed to allocate {} bytes at {}", size, current_backtrace());
    }

    auto lvl = log_level::debug;
    switch (dump_diagnostics_on_alloc_failure_kind.load(std::memory_order_relaxed)) {
        case alloc_failure_kind::none:
            lvl = log_level::debug;
            break;
        case alloc_failure_kind::critical:
            lvl = is_critical_alloc_section() ? log_level::error : log_level::debug;
            break;
        case alloc_failure_kind::all:
            lvl = log_level::error;
            break;
    }

    if (is_aborting) {
        // if we are about to abort, always report the memory diagnositics at error level
        lvl = log_level::error;
    }

    static thread_local logger::rate_limit rate_limit(std::chrono::seconds(10));
    dump_memory_diagnostics(lvl, rate_limit);


}

void on_allocation_failure(size_t size) {
    alloc_stats::increment(alloc_stats::types::failed_allocs);

    bool will_abort = !abort_on_alloc_failure_suppressed
            && abort_on_allocation_failure.load(std::memory_order_relaxed);

    maybe_dump_memory_diagnostics(size, will_abort);

    if (will_abort) {
        seastar_logger.error("Failed to allocate {} bytes", size);
        abort();
    }
}

sstring generate_memory_diagnostics_report() {
    seastar::internal::log_buf buf;
    auto it = buf.back_insert_begin();
    do_dump_memory_diagnostics(it);
    return sstring(buf.data(), buf.size());
}

static void trigger_error_injector() {
    on_alloc_point();
}

static bool try_trigger_error_injector() {
    try {
        on_alloc_point();
        return false;
    } catch (...) {
        return true;
    }
}

std::vector<allocation_site> sampled_memory_profile() {
    disable_backtrace_temporarily dbt;
    std::vector<allocation_site> ret(get_cpu_mem().asu.alloc_sites.begin(), get_cpu_mem().asu.alloc_sites.end());
    return ret;
}

size_t sampled_memory_profile(allocation_site* output, size_t size) {
    auto to_copy = std::min(size, get_cpu_mem().asu.alloc_sites.size());
    std::copy_n(get_cpu_mem().asu.alloc_sites.begin(), to_copy, output);
    return to_copy;
}

}

}

using namespace seastar::memory;

extern "C"
[[gnu::visibility("default")]]
[[gnu::used]]
void* malloc(size_t n) noexcept {
    if (try_trigger_error_injector()) {
        return nullptr;
    }
    return allocate(n);
}

extern "C"
[[gnu::alias("malloc")]]
[[gnu::visibility("default")]]
[[gnu::malloc]]
[[gnu::alloc_size(1)]]
#ifndef __clang__
[[gnu::leaf]]
#endif
void* __libc_malloc(size_t n) noexcept;

extern "C"
[[gnu::visibility("default")]]
[[gnu::used]]
void free(void* ptr) {
    seastar::memory::free(ptr);
}

extern "C"
[[gnu::alias("free")]]
[[gnu::visibility("default")]]
#ifndef __clang__
[[gnu::leaf]]
#endif
void __libc_free(void* obj) noexcept;

extern "C"
[[gnu::visibility("default")]]
void* calloc(size_t nmemb, size_t size) {
    if (try_trigger_error_injector()) {
        return nullptr;
    }
    auto s1 = __int128(nmemb) * __int128(size);
    SEASTAR_ASSERT(s1 == size_t(s1));
    size_t s = s1;
    auto p = malloc(s);
    if (p) {
        std::memset(p, 0, s);
    }
    return p;
}

extern "C"
[[gnu::alias("calloc")]]
[[gnu::visibility("default")]]
[[gnu::alloc_size(1, 2)]]
[[gnu::malloc]]
#ifndef __clang__
[[gnu::leaf]]
#endif
void* __libc_calloc(size_t n, size_t m) noexcept;

extern "C"
[[gnu::visibility("default")]]
void* realloc(void* ptr, size_t size) {
    if (try_trigger_error_injector()) {
        return nullptr;
    }
    if (ptr == nullptr) {
        // https://en.cppreference.com/w/cpp/memory/c/realloc
        // If ptr is a null pointer, the behavior is the same as calling std::malloc(new_size).
        return malloc(size);
    } else if (!is_seastar_memory(ptr)) {
        // original_realloc_func might be null when previous ctor allocates
        if (original_realloc_func) {
            return original_realloc_func(ptr, size);
        }
        // we can't realloc foreign memory without the original libc function
        abort();
    }
    // if we're here, it's a non-null seastar memory ptr
    auto old_size = ptr ? object_size(ptr) : 0;
    if (size == old_size) {
        return ptr;
    }
    if (size == 0) {
        ::free(ptr);
        return nullptr;
    }
    if (size < old_size && cpu_pages::is_local_pointer(ptr)) {
        // local pointers can sometimes be shrunk by returning freed
        // pages to the buddy allocator
        seastar::memory::shrink(ptr, size);
        return ptr;
    }

    // either a request to realloc larger than the existing allocation size
    // or a cross-shard pointer: in either case we allocate a new local
    // pointer and copy the contents
    auto nptr = malloc(size);
    if (!nptr) {
        return nptr;
    }
    if (ptr) {
        std::memcpy(nptr, ptr, std::min(size, old_size));
        ::free(ptr);
    }
    return nptr;
}

extern "C"
[[gnu::alias("realloc")]]
[[gnu::visibility("default")]]
[[gnu::alloc_size(2)]]
#ifndef __clang__
[[gnu::leaf]]
#endif
void* __libc_realloc(void* obj, size_t size) noexcept;

extern "C"
[[gnu::visibility("default")]]
[[gnu::used]]
#ifndef __clang__
[[gnu::leaf]]
#endif
[[gnu::nonnull(1)]]
int posix_memalign(void** ptr, size_t align, size_t size) noexcept {
    if (try_trigger_error_injector()) {
        return ENOMEM;
    }
    *ptr = allocate_aligned(align, size);
    if (!*ptr) {
        return ENOMEM;
    }
    return 0;
}

extern "C"
[[gnu::alias("posix_memalign")]]
[[gnu::visibility("default")]]
#ifndef __clang__
[[gnu::leaf]]
#endif
[[gnu::nonnull(1)]]
int __libc_posix_memalign(void** ptr, size_t align, size_t size) noexcept;

extern "C"
[[gnu::visibility("default")]]
[[gnu::malloc]]
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 30)
[[gnu::alloc_size(2)]]
#endif
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 35)
[[gnu::alloc_align(1)]]
#endif
void* memalign(size_t align, size_t size) noexcept {
    if (try_trigger_error_injector()) {
        return nullptr;
    }
    size = seastar::align_up(size, align);
    return allocate_aligned(align, size);
}

extern "C"
[[gnu::visibility("default")]]
void *aligned_alloc(size_t align, size_t size) noexcept {
    if (try_trigger_error_injector()) {
        return nullptr;
    }
    return allocate_aligned(align, size);
}

extern "C"
[[gnu::alias("memalign")]]
[[gnu::visibility("default")]]
[[gnu::malloc]]
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 30)
[[gnu::alloc_size(2)]]
#endif
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 35)
[[gnu::alloc_align(1)]]
#endif
void* __libc_memalign(size_t align, size_t size) noexcept;

extern "C"
[[gnu::visibility("default")]]
void cfree(void* obj) noexcept {
    return ::free(obj);
}

extern "C"
[[gnu::alias("cfree")]]
[[gnu::visibility("default")]]
void __libc_cfree(void* obj) noexcept;

extern "C"
[[gnu::visibility("default")]]
size_t malloc_usable_size(void* obj) {
    if (!is_seastar_memory(obj)) {
        return original_malloc_usable_size_func(obj);
    }
    return object_size(obj);
}

extern "C"
[[gnu::visibility("default")]]
int malloc_trim(size_t pad) {
    if (!is_reactor_thread) {
        return original_malloc_trim_func(pad);
    }
    return 0;
}

static inline
void* throw_if_null(void* ptr) {
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

extern "C++"
[[gnu::visibility("default")]]
void* operator new(size_t size) {
    trigger_error_injector();
    return throw_if_null(allocate(size));
}

extern "C++"
[[gnu::visibility("default")]]
void* operator new[](size_t size) {
    trigger_error_injector();
    return throw_if_null(allocate(size));
}

extern "C++"
[[gnu::visibility("default")]]
void operator delete(void* ptr) noexcept {
    seastar::memory::free(ptr);
}

extern "C++"
[[gnu::visibility("default")]]
void operator delete[](void* ptr) noexcept {
    seastar::memory::free(ptr);
}

extern "C++"
[[gnu::visibility("default")]]
void operator delete(void* ptr, size_t size) noexcept {
    seastar::memory::free(ptr, size);
}

extern "C++"
[[gnu::visibility("default")]]
void operator delete[](void* ptr, size_t size) noexcept {
    seastar::memory::free(ptr, size);
}

extern "C++"
[[gnu::visibility("default")]]
void* operator new(size_t size, std::nothrow_t) noexcept {
    if (try_trigger_error_injector()) {
        return nullptr;
    }
    return allocate(size);
}

extern "C++"
[[gnu::visibility("default")]]
void* operator new[](size_t size, std::nothrow_t) noexcept {
    return allocate(size);
}

extern "C++"
[[gnu::visibility("default")]]
void operator delete(void* ptr, std::nothrow_t) noexcept {
    seastar::memory::free(ptr);
}

extern "C++"
[[gnu::visibility("default")]]
void operator delete[](void* ptr, std::nothrow_t) noexcept {
    seastar::memory::free(ptr);
}

extern "C++"
[[gnu::visibility("default")]]
void operator delete(void* ptr, size_t size, std::nothrow_t) noexcept {
    seastar::memory::free(ptr, size);
}

extern "C++"
[[gnu::visibility("default")]]
void operator delete[](void* ptr, size_t size, std::nothrow_t) noexcept {
    seastar::memory::free(ptr, size);
}

extern "C++"
[[gnu::visibility("default")]]
void* operator new(size_t size, std::align_val_t a) {
    trigger_error_injector();
    auto ptr = allocate_aligned(size_t(a), size);
    return throw_if_null(ptr);
}

extern "C++"
[[gnu::visibility("default")]]
void* operator new[](size_t size, std::align_val_t a) {
    trigger_error_injector();
    auto ptr = allocate_aligned(size_t(a), size);
    return throw_if_null(ptr);
}

extern "C++"
[[gnu::visibility("default")]]
void* operator new(size_t size, std::align_val_t a, const std::nothrow_t&) noexcept {
    if (try_trigger_error_injector()) {
        return nullptr;
    }
    return allocate_aligned(size_t(a), size);
}

extern "C++"
[[gnu::visibility("default")]]
void* operator new[](size_t size, std::align_val_t a, const std::nothrow_t&) noexcept {
    if (try_trigger_error_injector()) {
        return nullptr;
    }
    return allocate_aligned(size_t(a), size);
}

extern "C++"
[[gnu::visibility("default")]]
void operator delete(void* ptr, std::align_val_t a) noexcept {
    seastar::memory::free(ptr);
}

extern "C++"
[[gnu::visibility("default")]]
void operator delete[](void* ptr, std::align_val_t a) noexcept {
    seastar::memory::free(ptr);
}

extern "C++"
[[gnu::visibility("default")]]
void operator delete(void* ptr, size_t size, std::align_val_t a) noexcept {
    if (ptr) {
        seastar::memory::free_aligned(ptr, size_t(a), size);
    }
}

extern "C++"
[[gnu::visibility("default")]]
void operator delete[](void* ptr, size_t size, std::align_val_t a) noexcept {
    if (ptr) {
        seastar::memory::free_aligned(ptr, size_t(a), size);
    }
}

extern "C++"
[[gnu::visibility("default")]]
void operator delete(void* ptr, std::align_val_t a, const std::nothrow_t&) noexcept {
    seastar::memory::free(ptr);
}

extern "C++"
[[gnu::visibility("default")]]
void operator delete[](void* ptr, std::align_val_t a, const std::nothrow_t&) noexcept {
    seastar::memory::free(ptr);
}

namespace seastar {

#else

namespace seastar {

namespace memory {

disable_backtrace_temporarily::disable_backtrace_temporarily() {
    (void)_disable_sampling;
}

void set_heap_profiling_sampling_rate(size_t) {
    seastar_logger.warn("Seastar compiled with default allocator, heap profiler not supported");
}

size_t get_heap_profiling_sample_rate() {
    return 0;
}

std::vector<allocation_site> sampled_memory_profile() {
    return {};
}

size_t sampled_memory_profile(allocation_site* output, size_t size) {
    return 0;
}

scoped_heap_profiling::scoped_heap_profiling(size_t sample_rate) noexcept {
    set_heap_profiling_sampling_rate(sample_rate); // let it print the warning
}

scoped_heap_profiling::~scoped_heap_profiling() {
}

void set_abort_on_allocation_failure(bool enabled) {
    if (enabled) {
        seastar_logger.warn("Seastar compiled with default allocator, will not abort on bad_alloc");
    }
}

bool is_abort_on_allocation_failure() {
    return false;
}

reclaimer::reclaimer(std::function<reclaiming_result ()> reclaim, reclaimer_scope) {
}

reclaimer::reclaimer(std::function<reclaiming_result (request)> reclaim, reclaimer_scope) {
}

reclaimer::~reclaimer() {
}

void set_reclaim_hook(std::function<void (std::function<void ()>)> hook) {
}

internal::numa_layout
configure(std::vector<resource::memory> m, bool mbind,
        bool transparent_hugepages,
        std::optional<std::string> hugepages_path) {
    return {};
}

void configure_minimal()
{}

statistics stats() {
    return statistics{0, 0, 0, 1 << 30, 1 << 30, 0, 0, 0, 0, 0, 0};
}

size_t free_memory() {
    return stats().free_memory();
}

bool drain_cross_cpu_freelist() {
    return false;
}

memory_layout get_memory_layout() {
    throw std::runtime_error("get_memory_layout() not supported");
}

size_t min_free_memory() {
    return 0;
}

void set_min_free_pages(size_t pages) {
    // Ignore, reclaiming not supported for default allocator.
}

void set_large_allocation_warning_threshold(size_t) {
    // Ignore, not supported for default allocator.
}

size_t get_large_allocation_warning_threshold() {
    // Ignore, not supported for default allocator.
    return std::numeric_limits<size_t>::max();
}

void disable_large_allocation_warning() {
    // Ignore, not supported for default allocator.
}


void set_dump_memory_diagnostics_on_alloc_failure_kind(alloc_failure_kind) {
    // Ignore, not supported for default allocator.
}

void set_dump_memory_diagnostics_on_alloc_failure_kind(std::string_view) {
    // Ignore, not supported for default allocator.
}

void set_additional_diagnostics_producer(noncopyable_function<void(memory_diagnostics_writer)>) {
    // Ignore, not supported for default allocator.
}

sstring generate_memory_diagnostics_report() {
    // Ignore, not supported for default allocator.
    return {};
}

}

}

namespace seastar {

#endif

/// \endcond

}
