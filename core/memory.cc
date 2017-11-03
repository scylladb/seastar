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

#include "cacheline.hh"
#include "memory.hh"
#include "reactor.hh"
#include "util/alloc_failure_injector.hh"

namespace seastar {

namespace memory {

static thread_local int abort_on_alloc_failure_suppressed = 0;

disable_abort_on_alloc_failure_temporarily::disable_abort_on_alloc_failure_temporarily() {
    ++abort_on_alloc_failure_suppressed;
}

disable_abort_on_alloc_failure_temporarily::~disable_abort_on_alloc_failure_temporarily() noexcept {
    --abort_on_alloc_failure_suppressed;
}

}

}

#ifndef DEFAULT_ALLOCATOR

#include "bitops.hh"
#include "align.hh"
#include "posix.hh"
#include "shared_ptr.hh"
#include <new>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <cassert>
#include <atomic>
#include <mutex>
#include <experimental/optional>
#include <functional>
#include <cstring>
#include <boost/intrusive/list.hpp>
#include <sys/mman.h>
#include "util/defer.hh"
#include "util/backtrace.hh"

#ifdef HAVE_NUMA
#include <numaif.h>
#endif

namespace seastar {

struct allocation_site {
    mutable size_t count = 0; // number of live objects allocated at backtrace.
    mutable size_t size = 0; // amount of bytes in live objects allocated at backtrace.
    mutable const allocation_site* next = nullptr;
    saved_backtrace backtrace;

    bool operator==(const allocation_site& o) const {
        return backtrace == o.backtrace;
    }

    bool operator!=(const allocation_site& o) const {
        return !(*this == o);
    }
};

}

namespace std {

template<>
struct hash<seastar::allocation_site> {
    size_t operator()(const seastar::allocation_site& bi) const {
        return std::hash<seastar::saved_backtrace>()(bi.backtrace);
    }
};

}

namespace seastar {

using allocation_site_ptr = const allocation_site*;

namespace memory {

seastar::logger seastar_memory_logger("seastar_memory");

static allocation_site_ptr get_allocation_site() __attribute__((unused));

static void on_allocation_failure(size_t size);

static constexpr unsigned cpu_id_shift = 36; // FIXME: make dynamic
static constexpr unsigned max_cpus = 256;

using pageidx = uint32_t;

struct page;
class page_list;

static std::atomic<bool> live_cpus[max_cpus];

static thread_local uint64_t g_allocs;
static thread_local uint64_t g_frees;
static thread_local uint64_t g_cross_cpu_frees;
static thread_local uint64_t g_reclaims;

using std::experimental::optional;

using allocate_system_memory_fn
        = std::function<mmap_area (optional<void*> where, size_t how_much)>;

namespace bi = boost::intrusive;

inline
unsigned object_cpu_id(const void* ptr) {
    return (reinterpret_cast<uintptr_t>(ptr) >> cpu_id_shift) & 0xff;
}

class page_list_link {
    uint32_t _prev;
    uint32_t _next;
    friend class page_list;
    friend void on_allocation_failure(size_t);
};

static char* mem_base() {
    static char* known;
    static std::once_flag flag;
    std::call_once(flag, [] {
        size_t alloc = size_t(1) << 44;
        auto r = ::mmap(NULL, 2 * alloc,
                    PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                    -1, 0);
        if (r == MAP_FAILED) {
            abort();
        }
        ::madvise(r, 2 * alloc, MADV_DONTDUMP);
        auto cr = reinterpret_cast<char*>(r);
        known = align_up(cr, alloc);
        ::munmap(cr, known - cr);
        ::munmap(known + alloc, cr + 2 * alloc - (known + alloc));
    });
    return known;
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
    page* find(uint32_t n_pages, page* ary) {
        auto n = _front;
        while (n && ary[n].span_size < n_pages) {
            n = ary[n].link._next;
        }
        if (!n) {
            return nullptr;
        }
        return &ary[n];
    }
    friend void on_allocation_failure(size_t);
};

class small_pool {
    struct span_sizes {
        uint8_t preferred;
        uint8_t fallback;
    };
    unsigned _object_size;
    span_sizes _span_sizes;
    free_object* _free = nullptr;
    size_t _free_count = 0;
    unsigned _min_free;
    unsigned _max_free;
    unsigned _pages_in_use = 0;
    page_list _span_list;
    static constexpr unsigned idx_frac_bits = 2;
public:
    explicit small_pool(unsigned object_size) noexcept;
    ~small_pool();
    void* allocate();
    void deallocate(void* object);
    unsigned object_size() const { return _object_size; }
    bool objects_page_aligned() const { return is_page_aligned(_object_size); }
    static constexpr unsigned size_to_idx(unsigned size);
    static constexpr unsigned idx_to_size(unsigned idx);
    allocation_site_ptr& alloc_site_holder(void* ptr);
private:
    void add_more_objects();
    void trim_free_list();
    friend void on_allocation_failure(size_t);
};

// index 0b0001'1100 -> size (1 << 4) + 0b11 << (4 - 2)

constexpr unsigned
small_pool::idx_to_size(unsigned idx) {
    return (((1 << idx_frac_bits) | (idx & ((1 << idx_frac_bits) - 1)))
              << (idx >> idx_frac_bits))
                  >> idx_frac_bits;
}

constexpr unsigned
small_pool::size_to_idx(unsigned size) {
    return ((log2floor(size) << idx_frac_bits) - ((1 << idx_frac_bits) - 1))
            + ((size - 1) >> (log2floor(size) - idx_frac_bits));
}

class small_pool_array {
public:
    static constexpr unsigned nr_small_pools = small_pool::size_to_idx(4 * page_size) + 1;
private:
    union u {
        small_pool a[nr_small_pools];
        u() {
            for (unsigned i = 0; i < nr_small_pools; ++i) {
                new (&a[i]) small_pool(small_pool::idx_to_size(i));
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
    = small_pool::idx_to_size(small_pool_array::nr_small_pools - 1);

constexpr size_t object_size_with_alloc_site(size_t size) {
#ifdef SEASTAR_HEAPPROF
    // For page-aligned sizes, allocation_site* lives in page::alloc_site, not with the object.
    static_assert(is_page_aligned(max_small_allocation), "assuming that max_small_allocation is page aligned so that we"
            " don't need to add allocation_site_ptr to objects of size close to it");
    size_t next_page_aligned_size = next_page_aligned(size);
    if (next_page_aligned_size - size > sizeof(allocation_site_ptr)) {
        size += sizeof(allocation_site_ptr);
    } else {
        return next_page_aligned_size;
    }
#endif
    return size;
}

#ifdef SEASTAR_HEAPPROF
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
    union pla {
        pla() {
            for (auto&& e : free_spans) {
                new (&e) page_list;
            }
        }
        ~pla() {
            // no destructor -- might be freeing after we die
        }
        page_list free_spans[nr_span_lists];  // contains spans with span_size >= 2^idx
    } fsu;
    small_pool_array small_pools;
    alignas(seastar::cache_line_size) std::atomic<cross_cpu_free_item*> xcpu_freelist;
    alignas(seastar::cache_line_size) std::vector<physical_address> virt_to_phys_map;
    static std::atomic<unsigned> cpu_id_gen;
    static cpu_pages* all_cpus[max_cpus];
    union asu {
        using alloc_sites_type = std::unordered_set<allocation_site>;
        asu() {
            new (&alloc_sites) alloc_sites_type();
        }
        ~asu() {} // alloc_sites live forever
        alloc_sites_type alloc_sites;
    } asu;
    allocation_site_ptr alloc_site_list_head = nullptr; // For easy traversal of asu.alloc_sites from scylla-gdb.py
    bool collect_backtrace = false;
    char* mem() { return memory; }

    void link(page_list& list, page* span);
    void unlink(page_list& list, page* span);
    struct trim {
        unsigned offset;
        unsigned nr_pages;
    };
    void maybe_reclaim();
    template <typename Trimmer>
    void* allocate_large_and_trim(unsigned nr_pages, Trimmer trimmer);
    void* allocate_large(unsigned nr_pages);
    void* allocate_large_aligned(unsigned align_pages, unsigned nr_pages);
    page* find_and_unlink_span(unsigned nr_pages);
    page* find_and_unlink_span_reclaiming(unsigned n_pages);
    void free_large(void* ptr);
    void free_span(pageidx start, uint32_t nr_pages);
    void free_span_no_merge(pageidx start, uint32_t nr_pages);
    void* allocate_small(unsigned size);
    void free(void* ptr);
    void free(void* ptr, size_t size);
    bool try_cross_cpu_free(void* ptr);
    void shrink(void* ptr, size_t new_size);
    void free_cross_cpu(unsigned cpu_id, void* ptr);
    bool drain_cross_cpu_freelist();
    size_t object_size(void* ptr);
    page* to_page(void* p) {
        return &pages[(reinterpret_cast<char*>(p) - mem()) / page_size];
    }

    bool is_initialized() const;
    bool initialize();
    reclaiming_result run_reclaimers(reclaimer_scope);
    void schedule_reclaim();
    void set_reclaim_hook(std::function<void (std::function<void ()>)> hook);
    void set_min_free_pages(size_t pages);
    void resize(size_t new_size, allocate_system_memory_fn alloc_sys_mem);
    void do_resize(size_t new_size, allocate_system_memory_fn alloc_sys_mem);
    void replace_memory_backing(allocate_system_memory_fn alloc_sys_mem);
    void init_virt_to_phys_map();
    void check_large_allocation(size_t size);
    void warn_large_allocation(size_t size);
    memory::memory_layout memory_layout();
    translation translate(const void* addr, size_t size);
    ~cpu_pages();
};

static thread_local cpu_pages cpu_mem;
std::atomic<unsigned> cpu_pages::cpu_id_gen;
cpu_pages* cpu_pages::all_cpus[max_cpus];

void set_heap_profiling_enabled(bool enable) {
    bool is_enabled = cpu_mem.collect_backtrace;
    if (enable) {
        if (!is_enabled) {
            seastar_logger.info("Enabling heap profiler");
        }
    } else {
        if (is_enabled) {
            seastar_logger.info("Disabling heap profiler");
        }
    }
    cpu_mem.collect_backtrace = enable;
}

// Free spans are store in the largest index i such that nr_pages >= 1 << i.
static inline
unsigned index_of(unsigned pages) {
    return std::numeric_limits<unsigned>::digits - count_leading_zeros(pages) - 1;
}

// Smallest index i such that all spans stored in the index are >= pages.
static inline
unsigned index_of_conservative(unsigned pages) {
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
    assert(nr_pages);
    nr_free_pages += nr_pages;
    auto span = &pages[span_start];
    auto span_end = &pages[span_start + nr_pages - 1];
    span->free = span_end->free = true;
    span->span_size = span_end->span_size = nr_pages;
    auto idx = index_of(nr_pages);
    link(fsu.free_spans[idx], span);
}

void cpu_pages::free_span(uint32_t span_start, uint32_t nr_pages) {
    page* before = &pages[span_start - 1];
    if (before->free) {
        auto b_size = before->span_size;
        assert(b_size);
        span_start -= b_size;
        nr_pages += b_size;
        nr_free_pages -= b_size;
        unlink(fsu.free_spans[index_of(b_size)], before - (b_size - 1));
    }
    page* after = &pages[span_start + nr_pages];
    if (after->free) {
        auto a_size = after->span_size;
        assert(a_size);
        nr_pages += a_size;
        nr_free_pages -= a_size;
        unlink(fsu.free_spans[index_of(a_size)], after);
    }
    free_span_no_merge(span_start, nr_pages);
}

page*
cpu_pages::find_and_unlink_span(unsigned n_pages) {
    auto idx = index_of_conservative(n_pages);
    auto orig_idx = idx;
    if (n_pages >= (2u << idx)) {
        throw std::bad_alloc();
    }
    while (idx < nr_span_lists && fsu.free_spans[idx].empty()) {
        ++idx;
    }
    if (idx == nr_span_lists) {
        if (initialize()) {
            return find_and_unlink_span(n_pages);
        }
        // Can smaller list possibly hold object?
        idx = index_of(n_pages);
        if (idx == orig_idx) {   // was exact power of two
            return nullptr;
        }
    }
    auto& list = fsu.free_spans[idx];
    page* span = list.find(n_pages, pages);
    if (!span) {
        return nullptr;
    }
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
        if (run_reclaimers(reclaimer_scope::sync) == reclaiming_result::reclaimed_nothing) {
            return nullptr;
        }
    }
}

void cpu_pages::maybe_reclaim() {
    if (nr_free_pages < current_min_free_pages) {
        drain_cross_cpu_freelist();
        run_reclaimers(reclaimer_scope::sync);
        if (nr_free_pages < current_min_free_pages) {
            schedule_reclaim();
        }
    }
}

template <typename Trimmer>
void*
cpu_pages::allocate_large_and_trim(unsigned n_pages, Trimmer trimmer) {
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
    trim t = trimmer(span_idx, nr_pages);
    if (t.offset) {
        free_span_no_merge(span_idx, t.offset);
        span_idx += t.offset;
        span_size -= t.offset;
        span = &pages[span_idx];

    }
    if (t.nr_pages < span_size) {
        free_span_no_merge(span_idx + t.nr_pages, span_size - t.nr_pages);
    }
    auto span_end = &pages[span_idx + t.nr_pages - 1];
    span->free = span_end->free = false;
    span->span_size = span_end->span_size = t.nr_pages;
    span->pool = nullptr;
#ifdef SEASTAR_HEAPPROF
    auto alloc_site = get_allocation_site();
    span->alloc_site = alloc_site;
    if (alloc_site) {
        ++alloc_site->count;
        alloc_site->size += span->span_size * page_size;
    }
#endif
    maybe_reclaim();
    return mem() + span_idx * page_size;
}

void
cpu_pages::warn_large_allocation(size_t size) {
    seastar_memory_logger.warn("oversized allocation: {} bytes, please report: at {}", size, current_backtrace());
    large_allocation_warning_threshold *= 1.618; // prevent spam
}

void
inline
cpu_pages::check_large_allocation(size_t size) {
    if (size > large_allocation_warning_threshold) {
        warn_large_allocation(size);
    }
}

void*
cpu_pages::allocate_large(unsigned n_pages) {
    check_large_allocation(n_pages * page_size);
    return allocate_large_and_trim(n_pages, [n_pages] (unsigned idx, unsigned n) {
        return trim{0, std::min(n, n_pages)};
    });
}

void*
cpu_pages::allocate_large_aligned(unsigned align_pages, unsigned n_pages) {
    check_large_allocation(n_pages * page_size);
    return allocate_large_and_trim(n_pages + align_pages - 1, [=] (unsigned idx, unsigned n) {
        return trim{align_up(idx, align_pages) - idx, n_pages};
    });
}

#ifdef SEASTAR_HEAPPROF

class disable_backtrace_temporarily {
    bool _old;
public:
    disable_backtrace_temporarily() {
        _old = cpu_mem.collect_backtrace;
        cpu_mem.collect_backtrace = false;
    }
    ~disable_backtrace_temporarily() {
        cpu_mem.collect_backtrace = _old;
    }
};

#else

struct disable_backtrace_temporarily {
    ~disable_backtrace_temporarily() {}
};

#endif

static
saved_backtrace get_backtrace() noexcept {
    disable_backtrace_temporarily dbt;
    return current_backtrace();
}

static
allocation_site_ptr get_allocation_site() {
    if (!cpu_mem.is_initialized() || !cpu_mem.collect_backtrace) {
        return nullptr;
    }
    disable_backtrace_temporarily dbt;
    allocation_site new_alloc_site;
    new_alloc_site.backtrace = get_backtrace();
    auto insert_result = cpu_mem.asu.alloc_sites.insert(std::move(new_alloc_site));
    allocation_site_ptr alloc_site = &*insert_result.first;
    if (insert_result.second) {
        alloc_site->next = cpu_mem.alloc_site_list_head;
        cpu_mem.alloc_site_list_head = alloc_site;
    }
    return alloc_site;
}

#ifdef SEASTAR_HEAPPROF

allocation_site_ptr&
small_pool::alloc_site_holder(void* ptr) {
    if (objects_page_aligned()) {
        return cpu_mem.to_page(ptr)->alloc_site;
    } else {
        return *reinterpret_cast<allocation_site_ptr*>(reinterpret_cast<char*>(ptr) + _object_size - sizeof(allocation_site_ptr));
    }
}

#endif

void*
cpu_pages::allocate_small(unsigned size) {
    auto idx = small_pool::size_to_idx(size);
    auto& pool = small_pools[idx];
    assert(size <= pool.object_size());
    auto ptr = pool.allocate();
#ifdef SEASTAR_HEAPPROF
    if (!ptr) {
        return nullptr;
    }
    allocation_site_ptr alloc_site = get_allocation_site();
    if (alloc_site) {
        ++alloc_site->count;
        alloc_site->size += pool.object_size();
    }
    new (&pool.alloc_site_holder(ptr)) allocation_site_ptr{alloc_site};
#endif
    return ptr;
}

void cpu_pages::free_large(void* ptr) {
    pageidx idx = (reinterpret_cast<char*>(ptr) - mem()) / page_size;
    page* span = &pages[idx];
#ifdef SEASTAR_HEAPPROF
    auto alloc_site = span->alloc_site;
    if (alloc_site) {
        --alloc_site->count;
        alloc_site->size -= span->span_size * page_size;
    }
#endif
    free_span(idx, span->span_size);
}

size_t cpu_pages::object_size(void* ptr) {
    pageidx idx = (reinterpret_cast<char*>(ptr) - mem()) / page_size;
    page* span = &pages[idx];
    if (span->pool) {
        auto s = span->pool->object_size();
#ifdef SEASTAR_HEAPPROF
        // We must not allow the object to be extended onto the allocation_site_ptr field.
        if (!span->pool->objects_page_aligned()) {
            s -= sizeof(allocation_site_ptr);
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
    ++g_cross_cpu_frees;
}

bool cpu_pages::drain_cross_cpu_freelist() {
    if (!xcpu_freelist.load(std::memory_order_relaxed)) {
        return false;
    }
    auto p = xcpu_freelist.exchange(nullptr, std::memory_order_acquire);
    while (p) {
        auto n = p->next;
        ++g_frees;
        free(p);
        p = n;
    }
    return true;
}

void cpu_pages::free(void* ptr) {
    page* span = to_page(ptr);
    if (span->pool) {
        small_pool& pool = *span->pool;
#ifdef SEASTAR_HEAPPROF
        allocation_site_ptr alloc_site = pool.alloc_site_holder(ptr);
        if (alloc_site) {
            --alloc_site->count;
            alloc_site->size -= pool.object_size();
        }
#endif
        pool.deallocate(ptr);
    } else {
        free_large(ptr);
    }
}

void cpu_pages::free(void* ptr, size_t size) {
    // match action on allocate() so hit the right pool
    if (size <= sizeof(free_object)) {
        size = sizeof(free_object);
    }
    if (size <= max_small_allocation) {
        size = object_size_with_alloc_site(size);
        auto pool = &small_pools[small_pool::size_to_idx(size)];
#ifdef SEASTAR_HEAPPROF
        allocation_site_ptr alloc_site = pool->alloc_site_holder(ptr);
        if (alloc_site) {
            --alloc_site->count;
            alloc_site->size -= pool->object_size();
        }
#endif
        pool->deallocate(ptr);
    } else {
        free_large(ptr);
    }
}

bool
cpu_pages::try_cross_cpu_free(void* ptr) {
    auto obj_cpu = object_cpu_id(ptr);
    if (obj_cpu != cpu_id) {
        free_cross_cpu(obj_cpu, ptr);
        return true;
    }
    return false;
}

void cpu_pages::shrink(void* ptr, size_t new_size) {
    auto obj_cpu = object_cpu_id(ptr);
    assert(obj_cpu == cpu_id);
    page* span = to_page(ptr);
    if (span->pool) {
        return;
    }
    size_t new_size_pages = align_up(new_size, page_size) / page_size;
    auto old_size_pages = span->span_size;
    assert(old_size_pages >= new_size_pages);
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
    free_span(idx + new_size_pages, old_size_pages - new_size_pages);
}

cpu_pages::~cpu_pages() {
    live_cpus[cpu_id].store(false, std::memory_order_relaxed);
}

bool cpu_pages::is_initialized() const {
    return bool(nr_pages);
}

bool cpu_pages::initialize() {
    if (is_initialized()) {
        return false;
    }
    cpu_id = cpu_id_gen.fetch_add(1, std::memory_order_relaxed);
    assert(cpu_id < max_cpus);
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
    ::madvise(base, size, MADV_HUGEPAGE);
    pages = reinterpret_cast<page*>(base);
    memory = base;
    nr_pages = size / page_size;
    // we reserve the end page so we don't have to special case
    // the last span.
    auto reserved = align_up(sizeof(page) * (nr_pages + 1), page_size) / page_size;
    for (pageidx i = 0; i < reserved; ++i) {
        pages[i].free = false;
    }
    pages[nr_pages].free = false;
    free_span_no_merge(reserved, nr_pages - reserved);
    live_cpus[cpu_id].store(true, std::memory_order_relaxed);
    return true;
}

mmap_area
allocate_anonymous_memory(std::experimental::optional<void*> where, size_t how_much) {
    return mmap_anonymous(where.value_or(nullptr),
            how_much,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | (where ? MAP_FIXED : 0));
}

mmap_area
allocate_hugetlbfs_memory(file_desc& fd, std::experimental::optional<void*> where, size_t how_much) {
    auto pos = fd.size();
    fd.truncate(pos + how_much);
    auto ret = fd.map(
            how_much,
            PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE | (where ? MAP_FIXED : 0),
            pos,
            where.value_or(nullptr));
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
    alloc_sys_mem({old_mem}, bytes).release();
    std::memcpy(old_mem, relocated_old_mem.get(), bytes);
}

void cpu_pages::init_virt_to_phys_map() {
    auto nr_entries = nr_pages / (huge_page_size / page_size);
    virt_to_phys_map.resize(nr_entries);
    auto fd = file_desc::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    for (size_t i = 0; i != nr_entries; ++i) {
        uint64_t entry = 0;
        auto phys = std::numeric_limits<physical_address>::max();
        auto pfn = reinterpret_cast<uintptr_t>(mem() + i * huge_page_size) / page_size;
        fd.pread(&entry, 8, pfn * 8);
        assert(entry & 0x8000'0000'0000'0000);
        phys = (entry & 0x007f'ffff'ffff'ffff) << page_bits;
        virt_to_phys_map[i] = phys;
    }
}

translation
cpu_pages::translate(const void* addr, size_t size) {
    auto a = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mem());
    auto pfn = a / huge_page_size;
    if (pfn >= virt_to_phys_map.size()) {
        return {};
    }
    auto phys = virt_to_phys_map[pfn];
    if (phys == std::numeric_limits<physical_address>::max()) {
        return {};
    }
    auto translation_size = align_up(a + 1, huge_page_size) - a;
    size = std::min(size, translation_size);
    phys += a & (huge_page_size - 1);
    return translation{phys, size};
}

void cpu_pages::do_resize(size_t new_size, allocate_system_memory_fn alloc_sys_mem) {
    auto new_pages = new_size / page_size;
    if (new_pages <= nr_pages) {
        return;
    }
    auto old_size = nr_pages * page_size;
    auto mmap_start = memory + old_size;
    auto mmap_size = new_size - old_size;
    auto mem = alloc_sys_mem({mmap_start}, mmap_size);
    mem.release();
    ::madvise(mmap_start, mmap_size, MADV_HUGEPAGE);
    // one past last page structure is a sentinel
    auto new_page_array_pages = align_up(sizeof(page[new_pages + 1]), page_size) / page_size;
    auto new_page_array
        = reinterpret_cast<page*>(allocate_large(new_page_array_pages));
    if (!new_page_array) {
        throw std::bad_alloc();
    }
    std::copy(pages, pages + nr_pages, new_page_array);
    // mark new one-past-last page as taken to avoid boundary conditions
    new_page_array[new_pages].free = false;
    auto old_pages = reinterpret_cast<char*>(pages);
    auto old_nr_pages = nr_pages;
    auto old_pages_size = align_up(sizeof(page[nr_pages + 1]), page_size);
    pages = new_page_array;
    nr_pages = new_pages;
    auto old_pages_start = (old_pages - memory) / page_size;
    if (old_pages_start == 0) {
        // keep page 0 allocated
        old_pages_start = 1;
        old_pages_size -= page_size;
    }
    if (old_pages_size != 0) {
        free_span(old_pages_start, old_pages_size / page_size);
    }
    free_span(old_nr_pages, new_pages - old_nr_pages);
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

reclaiming_result cpu_pages::run_reclaimers(reclaimer_scope scope) {
    auto target = std::max(nr_free_pages + 1, min_free_pages);
    reclaiming_result result = reclaiming_result::reclaimed_nothing;
    while (nr_free_pages < target) {
        bool made_progress = false;
        ++g_reclaims;
        for (auto&& r : reclaimers) {
            if (r->scope() >= scope) {
                made_progress |= r->do_reclaim() == reclaiming_result::reclaimed_something;
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
                run_reclaimers(reclaimer_scope::async);
            } catch (...) {
                current_min_free_pages = min_free_pages;
                throw;
            }
        }
        current_min_free_pages = min_free_pages;
    });
}

memory::memory_layout cpu_pages::memory_layout() {
    assert(is_initialized());
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

small_pool::small_pool(unsigned object_size) noexcept
    : _object_size(object_size) {
    unsigned span_size = 1;
    auto span_bytes = [&] { return span_size * page_size; };
    auto waste = [&] { return (span_bytes() % _object_size) / (1.0 * span_bytes()); };
    while (object_size > span_bytes()) {
        ++span_size;
    }
    _span_sizes.fallback = span_size;
    span_size = 1;
    while (_object_size > span_bytes()
            || (span_size < 32 && waste() > 0.05)
            || (span_bytes() / object_size < 4)) {
        ++span_size;
    }
    _span_sizes.preferred = span_size;
    _max_free = std::max<unsigned>(100, span_bytes() * 2 / _object_size);
    _min_free = _max_free / 2;
}

small_pool::~small_pool() {
    _min_free = _max_free = 0;
    trim_free_list();
}

// Should not throw in case of running out of memory to avoid infinite recursion,
// becaue throwing std::bad_alloc requires allocation. __cxa_allocate_exception
// falls back to the emergency pool in case malloc() returns nullptr.
void*
small_pool::allocate() {
    if (!_free) {
        add_more_objects();
    }
    if (!_free) {
        return nullptr;
    }
    auto* obj = _free;
    _free = _free->next;
    --_free_count;
    return obj;
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

void
small_pool::add_more_objects() {
    auto goal = (_min_free + _max_free) / 2;
    while (!_span_list.empty() && _free_count < goal) {
        page& span = _span_list.front(cpu_mem.pages);
        _span_list.pop_front(cpu_mem.pages);
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
        disable_backtrace_temporarily dbt;
        auto span_size = _span_sizes.preferred;
        auto data = reinterpret_cast<char*>(cpu_mem.allocate_large(span_size));
        if (!data) {
            span_size = _span_sizes.fallback;
            data = reinterpret_cast<char*>(cpu_mem.allocate_large(span_size));
            if (!data) {
                return;
            }
        }
        _pages_in_use += span_size;
        auto span = cpu_mem.to_page(data);
        for (unsigned i = 0; i < span_size; ++i) {
            span[i].offset_in_span = i;
            span[i].pool = this;
        }
        span->nr_small_alloc = 0;
        span->freelist = nullptr;
        for (unsigned offset = 0; offset <= span_size * page_size - _object_size; offset += _object_size) {
            auto h = reinterpret_cast<free_object*>(data + offset);
            h->next = _free;
            _free = h;
            ++_free_count;
            ++span->nr_small_alloc;
        }
    }
}

void
small_pool::trim_free_list() {
    auto goal = (_min_free + _max_free) / 2;
    while (_free && _free_count > goal) {
        auto obj = _free;
        _free = _free->next;
        --_free_count;
        page* span = cpu_mem.to_page(obj);
        span -= span->offset_in_span;
        if (!span->freelist) {
            new (&span->link) page_list_link();
            _span_list.push_front(cpu_mem.pages, *span);
        }
        obj->next = span->freelist;
        span->freelist = obj;
        if (--span->nr_small_alloc == 0) {
            _pages_in_use -= span->span_size;
            _span_list.erase(cpu_mem.pages, *span);
            cpu_mem.free_span(span - cpu_mem.pages, span->span_size);
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

void* allocate_large(size_t size) {
    abort_on_underflow(size);
    unsigned size_in_pages = (size + page_size - 1) >> page_bits;
    if ((size_t(size_in_pages) << page_bits) < size) {
        throw std::bad_alloc();
    }
    return cpu_mem.allocate_large(size_in_pages);

}

void* allocate_large_aligned(size_t align, size_t size) {
    abort_on_underflow(size);
    unsigned size_in_pages = (size + page_size - 1) >> page_bits;
    unsigned align_in_pages = std::max(align, page_size) >> page_bits;
    return cpu_mem.allocate_large_aligned(align_in_pages, size_in_pages);
}

void free_large(void* ptr) {
    return cpu_mem.free_large(ptr);
}

size_t object_size(void* ptr) {
    return cpu_pages::all_cpus[object_cpu_id(ptr)]->object_size(ptr);
}

void* allocate(size_t size) {
    on_alloc_point();
    if (size <= sizeof(free_object)) {
        size = sizeof(free_object);
    }
    void* ptr;
    if (size <= max_small_allocation) {
        size = object_size_with_alloc_site(size);
        ptr = cpu_mem.allocate_small(size);
    } else {
        ptr = allocate_large(size);
    }
    if (!ptr) {
        on_allocation_failure(size);
    }
    ++g_allocs;
    return ptr;
}

void* allocate_aligned(size_t align, size_t size) {
    on_alloc_point();
    size = std::max(size, align);
    if (size <= sizeof(free_object)) {
        size = sizeof(free_object);
    }
    void* ptr;
    if (size <= max_small_allocation && align <= page_size) {
        // Our small allocator only guarantees alignment for power-of-two
        // allocations which are not larger than a page.
        size = 1 << log2ceil(object_size_with_alloc_site(size));
        ptr = cpu_mem.allocate_small(size);
    } else {
        ptr = allocate_large_aligned(align, size);
    }
    if (!ptr) {
        on_allocation_failure(size);
    }
    ++g_allocs;
    return ptr;
}

void free(void* obj) {
    if (cpu_mem.try_cross_cpu_free(obj)) {
        return;
    }
    ++g_frees;
    cpu_mem.free(obj);
}

void free(void* obj, size_t size) {
    if (cpu_mem.try_cross_cpu_free(obj)) {
        return;
    }
    ++g_frees;
    cpu_mem.free(obj, size);
}

void shrink(void* obj, size_t new_size) {
    ++g_frees;
    ++g_allocs; // keep them balanced
    cpu_mem.shrink(obj, new_size);
}

void set_reclaim_hook(std::function<void (std::function<void ()>)> hook) {
    cpu_mem.set_reclaim_hook(hook);
}

reclaimer::reclaimer(reclaim_fn reclaim, reclaimer_scope scope)
    : _reclaim(std::move(reclaim))
    , _scope(scope) {
    cpu_mem.reclaimers.push_back(this);
}

reclaimer::~reclaimer() {
    auto& r = cpu_mem.reclaimers;
    r.erase(std::find(r.begin(), r.end(), this));
}

void set_large_allocation_warning_threshold(size_t threshold) {
    cpu_mem.large_allocation_warning_threshold = threshold;
}

size_t get_large_allocation_warning_threshold() {
    return cpu_mem.large_allocation_warning_threshold;
}

void disable_large_allocation_warning() {
    cpu_mem.large_allocation_warning_threshold = std::numeric_limits<size_t>::max();
}

void configure(std::vector<resource::memory> m, bool mbind,
        optional<std::string> hugetlbfs_path) {
    size_t total = 0;
    for (auto&& x : m) {
        total += x.bytes;
    }
    allocate_system_memory_fn sys_alloc = allocate_anonymous_memory;
    if (hugetlbfs_path) {
        // std::function is copyable, but file_desc is not, so we must use
        // a shared_ptr to allow sys_alloc to be copied around
        auto fdp = make_lw_shared<file_desc>(file_desc::temporary(*hugetlbfs_path));
        sys_alloc = [fdp] (optional<void*> where, size_t how_much) {
            return allocate_hugetlbfs_memory(*fdp, where, how_much);
        };
        cpu_mem.replace_memory_backing(sys_alloc);
    }
    cpu_mem.resize(total, sys_alloc);
    size_t pos = 0;
    for (auto&& x : m) {
#ifdef HAVE_NUMA
        unsigned long nodemask = 1UL << x.nodeid;
        if (mbind) {
            auto r = ::mbind(cpu_mem.mem() + pos, x.bytes,
                            MPOL_PREFERRED,
                            &nodemask, std::numeric_limits<unsigned long>::digits,
                            MPOL_MF_MOVE);

            if (r == -1) {
                char err[1000] = {};
                strerror_r(errno, err, sizeof(err));
                std::cerr << "WARNING: unable to mbind shard memory; performance may suffer: "
                        << err << std::endl;
            }
        }
#endif
        pos += x.bytes;
    }
    if (hugetlbfs_path) {
        cpu_mem.init_virt_to_phys_map();
    }
}

statistics stats() {
    return statistics{g_allocs, g_frees, g_cross_cpu_frees,
        cpu_mem.nr_pages * page_size, cpu_mem.nr_free_pages * page_size, g_reclaims};
}

bool drain_cross_cpu_freelist() {
    return cpu_mem.drain_cross_cpu_freelist();
}

translation
translate(const void* addr, size_t size) {
    auto cpu_id = object_cpu_id(addr);
    if (cpu_id >= max_cpus) {
        return {};
    }
    auto cp = cpu_pages::all_cpus[cpu_id];
    if (!cp) {
        return {};
    }
    return cp->translate(addr, size);
}

memory_layout get_memory_layout() {
    return cpu_mem.memory_layout();
}

size_t min_free_memory() {
    return cpu_mem.min_free_pages * page_size;
}

void set_min_free_pages(size_t pages) {
    cpu_mem.set_min_free_pages(pages);
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

void enable_abort_on_allocation_failure() {
    abort_on_allocation_failure.store(true, std::memory_order_seq_cst);
}

void on_allocation_failure(size_t size) {
    if (!report_on_alloc_failure_suppressed &&
            // report even suppressed failures if trace level is enabled
            (seastar_memory_logger.is_enabled(seastar::log_level::trace) ||
                    (seastar_memory_logger.is_enabled(seastar::log_level::debug) && !abort_on_alloc_failure_suppressed))) {
        disable_report_on_alloc_failure_temporarily guard;
        seastar_memory_logger.debug("Failed to allocate {} bytes at {}", size, current_backtrace());
        auto free_mem = cpu_mem.nr_free_pages * page_size;
        auto total_mem = cpu_mem.nr_pages * page_size;
        seastar_memory_logger.debug("Used memory: {} Free memory: {} Total memory: {}", total_mem - free_mem, free_mem, total_mem);
        seastar_memory_logger.debug("Small pools:");
        seastar_memory_logger.debug("objsz spansz usedobj   memory       wst%");
        for (unsigned i = 0; i < cpu_mem.small_pools.nr_small_pools; i++) {
            auto& sp = cpu_mem.small_pools[i];
            auto use_count = sp._pages_in_use * page_size / sp.object_size() - sp._free_count;
            auto memory = sp._pages_in_use * page_size;
            auto wasted_percent = memory ? sp._free_count * sp.object_size() * 100.0 / memory : 0;
            seastar_memory_logger.debug("{} {} {} {} {}", sp.object_size(), sp._span_sizes.preferred * page_size, use_count, memory, wasted_percent);
        }
        seastar_memory_logger.debug("Page spans:");
        seastar_memory_logger.debug("index size [B]     free [B]");
        for (unsigned i = 0; i< cpu_mem.nr_span_lists; i++) {
            auto& span_list = cpu_mem.fsu.free_spans[i];
            auto front = span_list._front;
            uint32_t total = 0;
            while(front) {
                auto& span = cpu_mem.pages[front];
                total += span.span_size;
                front = span.link._next;
            }
            seastar_memory_logger.debug("{} {} {}", i, (1<<i) * page_size, total * page_size);
        }
    }

    if (!abort_on_alloc_failure_suppressed
            && abort_on_allocation_failure.load(std::memory_order_relaxed)) {
        seastar_logger.error("Failed to allocate {} bytes", size);
        abort();
    }
}

}

}

using namespace seastar::memory;

extern "C"
[[gnu::visibility("default")]]
[[gnu::externally_visible]]
void* malloc(size_t n) throw () {
    try {
        return allocate(n);
    } catch (std::bad_alloc& ba) {
        on_allocation_failure(n);
        return nullptr;
    }
}

extern "C"
[[gnu::alias("malloc")]]
[[gnu::visibility("default")]]
void* __libc_malloc(size_t n) throw ();

extern "C"
[[gnu::visibility("default")]]
[[gnu::externally_visible]]
void free(void* ptr) {
    if (ptr) {
        seastar::memory::free(ptr);
    }
}

extern "C"
[[gnu::alias("free")]]
[[gnu::visibility("default")]]
void* __libc_free(void* obj) throw ();

extern "C"
[[gnu::visibility("default")]]
void* calloc(size_t nmemb, size_t size) {
    auto s1 = __int128(nmemb) * __int128(size);
    assert(s1 == size_t(s1));
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
void* __libc_calloc(size_t n, size_t m) throw ();

extern "C"
[[gnu::visibility("default")]]
void* realloc(void* ptr, size_t size) {
    seastar::memory::on_alloc_point();
    auto old_size = ptr ? object_size(ptr) : 0;
    if (size == old_size) {
        return ptr;
    }
    if (size == 0) {
        ::free(ptr);
        return nullptr;
    }
    if (size < old_size) {
        seastar::memory::shrink(ptr, size);
        return ptr;
    }
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
void* __libc_realloc(void* obj, size_t size) throw ();

extern "C"
[[gnu::visibility("default")]]
[[gnu::externally_visible]]
int posix_memalign(void** ptr, size_t align, size_t size) {
    try {
        *ptr = allocate_aligned(align, size);
        if (!*ptr) {
            return ENOMEM;
        }
        return 0;
    } catch (std::bad_alloc&) {
        on_allocation_failure(size);
        return ENOMEM;
    }
}

extern "C"
[[gnu::alias("posix_memalign")]]
[[gnu::visibility("default")]]
int __libc_posix_memalign(void** ptr, size_t align, size_t size) throw ();

extern "C"
[[gnu::visibility("default")]]
void* memalign(size_t align, size_t size) {
    try {
        return allocate_aligned(align, size);
    } catch (std::bad_alloc&) {
        return NULL;
    }
}

extern "C"
[[gnu::visibility("default")]]
void *aligned_alloc(size_t align, size_t size) {
    try {
        return allocate_aligned(align, size);
    } catch (std::bad_alloc&) {
        return NULL;
    }
}

extern "C"
[[gnu::alias("memalign")]]
[[gnu::visibility("default")]]
void* __libc_memalign(size_t align, size_t size);

extern "C"
[[gnu::visibility("default")]]
void cfree(void* obj) {
    return ::free(obj);
}

extern "C"
[[gnu::alias("cfree")]]
[[gnu::visibility("default")]]
void __libc_cfree(void* obj);

extern "C"
[[gnu::visibility("default")]]
size_t malloc_usable_size(void* obj) {
    return object_size(obj);
}

extern "C"
[[gnu::visibility("default")]]
int malloc_trim(size_t pad) {
    return 0;
}

static inline
void* throw_if_null(void* ptr) {
    if (!ptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

[[gnu::visibility("default")]]
void* operator new(size_t size) {
    if (size == 0) {
        size = 1;
    }
    return throw_if_null(allocate(size));
}

[[gnu::visibility("default")]]
void* operator new[](size_t size) {
    if (size == 0) {
        size = 1;
    }
    return throw_if_null(allocate(size));
}

[[gnu::visibility("default")]]
void operator delete(void* ptr) throw () {
    if (ptr) {
        seastar::memory::free(ptr);
    }
}

[[gnu::visibility("default")]]
void operator delete[](void* ptr) throw () {
    if (ptr) {
        seastar::memory::free(ptr);
    }
}

[[gnu::visibility("default")]]
void operator delete(void* ptr, size_t size) throw () {
    if (ptr) {
        seastar::memory::free(ptr, size);
    }
}

[[gnu::visibility("default")]]
void operator delete[](void* ptr, size_t size) throw () {
    if (ptr) {
        seastar::memory::free(ptr, size);
    }
}

[[gnu::visibility("default")]]
void* operator new(size_t size, std::nothrow_t) throw () {
    if (size == 0) {
        size = 1;
    }
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}

[[gnu::visibility("default")]]
void* operator new[](size_t size, std::nothrow_t) throw () {
    if (size == 0) {
        size = 1;
    }
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}

[[gnu::visibility("default")]]
void operator delete(void* ptr, std::nothrow_t) throw () {
    if (ptr) {
        seastar::memory::free(ptr);
    }
}

[[gnu::visibility("default")]]
void operator delete[](void* ptr, std::nothrow_t) throw () {
    if (ptr) {
        seastar::memory::free(ptr);
    }
}

[[gnu::visibility("default")]]
void operator delete(void* ptr, size_t size, std::nothrow_t) throw () {
    if (ptr) {
        seastar::memory::free(ptr, size);
    }
}

[[gnu::visibility("default")]]
void operator delete[](void* ptr, size_t size, std::nothrow_t) throw () {
    if (ptr) {
        seastar::memory::free(ptr, size);
    }
}

void* operator new(size_t size, seastar::with_alignment wa) {
    return allocate_aligned(wa.alignment(), size);
}

void* operator new[](size_t size, seastar::with_alignment wa) {
    return allocate_aligned(wa.alignment(), size);
}

void operator delete(void* ptr, seastar::with_alignment wa) {
    // only called for matching operator new, so we know ptr != nullptr
    return seastar::memory::free(ptr);
}

void operator delete[](void* ptr, seastar::with_alignment wa) {
    // only called for matching operator new, so we know ptr != nullptr
    return seastar::memory::free(ptr);
}

namespace seastar {

#else

namespace seastar {

namespace memory {

void set_heap_profiling_enabled(bool enabled) {
    seastar_logger.warn("Seastar compiled with default allocator, heap profiler not supported");
}

void enable_abort_on_allocation_failure() {
    seastar_logger.warn("Seastar compiled with default allocator, will not abort on bad_alloc");
}

reclaimer::reclaimer(reclaim_fn reclaim, reclaimer_scope) {
}

reclaimer::~reclaimer() {
}

void set_reclaim_hook(std::function<void (std::function<void ()>)> hook) {
}

void configure(std::vector<resource::memory> m, bool mbind, std::experimental::optional<std::string> hugepages_path) {
}

statistics stats() {
    return statistics{0, 0, 0, 1 << 30, 1 << 30, 0};
}

bool drain_cross_cpu_freelist() {
    return false;
}

translation
translate(const void* addr, size_t size) {
    return {};
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

}

}

void* operator new(size_t size, seastar::with_alignment wa) {
    void* ret;
    if (posix_memalign(&ret, wa.alignment(), size) != 0) {
        throw std::bad_alloc();
    }
    return ret;
}

void* operator new[](size_t size, seastar::with_alignment wa) {
    void* ret;
    if (posix_memalign(&ret, wa.alignment(), size) != 0) {
        throw std::bad_alloc();
    }
    return ret;
}

void operator delete(void* ptr, seastar::with_alignment wa) {
    return ::free(ptr);
}

void operator delete[](void* ptr, seastar::with_alignment wa) {
    return ::free(ptr);
}

namespace seastar {

#endif

/// \endcond

}

