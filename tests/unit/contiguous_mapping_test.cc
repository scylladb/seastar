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

#include <seastar/core/aligned_buffer.hh>
#include <seastar/core/memory.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/util/log.hh>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using namespace seastar;

static logger testlog("contiguous_mapping_test");

// Returns the shard's memory layout, or nothing if the shard's memory is not
// backed by a file (the default allocator is disabled, memfd backing is off, or
// hugetlbfs is used) and contiguous_mapping therefore cannot work.
static std::optional<memory::memory_layout> file_backed_memory_layout() {
    try {
        auto layout = memory::get_memory_layout();
        if (!layout.memfd) {
            return std::nullopt;
        }
        return layout;
    } catch (const std::runtime_error&) {
        // get_memory_layout() is not supported with the default allocator.
        return std::nullopt;
    }
}

// A page-aligned allocation to be aliased by a contiguous_mapping.
class chunk {
    std::unique_ptr<char[], free_deleter> _mem;
    size_t _size;
public:
    explicit chunk(size_t size)
            : _mem(allocate_aligned_buffer<char>(size, memory::page_size))
            , _size(size) {
    }
    std::span<char> range() const noexcept { return {_mem.get(), _size}; }
    char* data() const noexcept { return _mem.get(); }
    size_t size() const noexcept { return _size; }
};

// Tests whether \p p can be read, without risking a fatal SIGSEGV: a syscall
// reading from an inaccessible address fails with EFAULT instead of faulting.
static bool readable(const char* p) {
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        throw std::system_error(errno, std::system_category(), "pipe2");
    }
    auto n = ::write(fds[1], p, 1);
    auto err = errno;
    ::close(fds[0]);
    ::close(fds[1]);
    if (n == 1) {
        return true;
    }
    BOOST_REQUIRE_EQUAL(err, EFAULT);
    return false;
}

SEASTAR_THREAD_TEST_CASE(test_map_contiguous_aliases_its_ranges) {
    if (!file_backed_memory_layout()) {
        testlog.info("skipping: the shard's memory is not backed by a file");
        return;
    }
    constexpr size_t chunk_size = 128 * 1024;
    constexpr unsigned nr_chunks = 8;
    auto chunks = std::vector<chunk>();
    for (unsigned i = 0; i < nr_chunks; ++i) {
        chunks.emplace_back(chunk_size);
    }
    // Map the chunks in an order unrelated to their addresses, to be sure the
    // mapping follows the requested order rather than the address order.
    auto ranges = std::vector<std::span<char>>();
    for (unsigned i = 0; i < nr_chunks; ++i) {
        ranges.push_back(chunks[(i * 5) % nr_chunks].range());
    }
    auto mapping = memory::map_contiguous(ranges);
    BOOST_REQUIRE_EQUAL(mapping.size(), nr_chunks * chunk_size);
    BOOST_REQUIRE_EQUAL(mapping.capacity(), nr_chunks * chunk_size);
    BOOST_REQUIRE_EQUAL(reinterpret_cast<uintptr_t>(mapping.data()) % memory::page_size, 0u);
    // A store through the original mapping is visible in the contiguous one...
    for (unsigned i = 0; i < nr_chunks; ++i) {
        std::memset(ranges[i].data(), 'a' + i, ranges[i].size());
    }
    for (unsigned i = 0; i < nr_chunks; ++i) {
        auto alias = mapping.range().subspan(i * chunk_size, chunk_size);
        BOOST_REQUIRE(std::all_of(alias.begin(), alias.end(), [c0 = char('a' + i)] (char c) { return c == c0; }));
    }
    // ... and vice versa.
    for (unsigned i = 0; i < nr_chunks; ++i) {
        std::memset(mapping.data() + i * chunk_size, 'A' + i, chunk_size);
    }
    for (unsigned i = 0; i < nr_chunks; ++i) {
        auto range = ranges[i];
        BOOST_REQUIRE(std::all_of(range.begin(), range.end(), [c0 = char('A' + i)] (char c) { return c == c0; }));
    }
}

SEASTAR_THREAD_TEST_CASE(test_contiguous_mapping_grows_in_place) {
    if (!file_backed_memory_layout()) {
        testlog.info("skipping: the shard's memory is not backed by a file");
        return;
    }
    constexpr size_t chunk_size = 128 * 1024;
    auto mapping = memory::contiguous_mapping(4 * chunk_size);
    BOOST_REQUIRE_EQUAL(mapping.size(), 0u);
    BOOST_REQUIRE_EQUAL(mapping.capacity(), 4 * chunk_size);
    auto chunks = std::vector<chunk>();
    auto base = mapping.data();
    for (unsigned i = 0; i < 4; ++i) {
        chunks.emplace_back(chunk_size);
        std::memset(chunks.back().data(), 'a' + i, chunk_size);
        auto range = chunks.back().range();
        mapping.append(std::span(&range, 1));
        // Growing does not relocate the mapping.
        BOOST_REQUIRE(mapping.data() == base);
        BOOST_REQUIRE_EQUAL(mapping.size(), (i + 1) * chunk_size);
        // Everything mapped so far is still there.
        for (unsigned j = 0; j <= i; ++j) {
            BOOST_REQUIRE_EQUAL(mapping.data()[j * chunk_size], char('a' + j));
        }
        // The rest of the reservation is inaccessible.
        if (i != 3) {
            BOOST_REQUIRE(!readable(mapping.data() + mapping.size()));
        }
    }
    auto one_too_many = chunk(chunk_size);
    auto range = one_too_many.range();
    BOOST_REQUIRE_THROW(mapping.append(std::span(&range, 1)), std::bad_alloc);
}

SEASTAR_THREAD_TEST_CASE(test_contiguous_mapping_rejects_bad_ranges) {
    auto layout = file_backed_memory_layout();
    if (!layout) {
        testlog.info("skipping: the shard's memory is not backed by a file");
        return;
    }
    auto mapping = memory::contiguous_mapping(4 * memory::page_size);
    auto c = chunk(2 * memory::page_size);
    auto misaligned_start = std::span<char>(c.data() + 1, memory::page_size);
    BOOST_REQUIRE_THROW(mapping.append(std::span(&misaligned_start, 1)), std::invalid_argument);
    auto misaligned_size = std::span<char>(c.data(), memory::page_size + 1);
    BOOST_REQUIRE_THROW(mapping.append(std::span(&misaligned_size, 1)), std::invalid_argument);
    // Static storage is not part of the shard's memory (nor is the stack of a
    // seastar thread, which is allocated by the shard's allocator).
    alignas(memory::page_size) static char outside_the_heap[memory::page_size];
    auto foreign = std::span<char>(outside_the_heap, memory::page_size);
    BOOST_REQUIRE_THROW(mapping.append(std::span(&foreign, 1)), std::invalid_argument);
    auto past_the_end = std::span<char>(reinterpret_cast<char*>(layout->end), memory::page_size);
    BOOST_REQUIRE_THROW(mapping.append(std::span(&past_the_end, 1)), std::invalid_argument);
    auto way_past_the_end = std::span<char>(reinterpret_cast<char*>(layout->end + (1 << 30)),
            memory::page_size);
    BOOST_REQUIRE_THROW(mapping.append(std::span(&way_past_the_end, 1)), std::invalid_argument);
    // A rejected request leaves the mapping alone.
    BOOST_REQUIRE_EQUAL(mapping.size(), 0u);
    BOOST_REQUIRE_THROW(memory::contiguous_mapping(memory::page_size + 1), std::invalid_argument);
}

// A stand-in for a wasmtime guest memory, following the contract of
// wasmtime::LinearMemory (wasmtime_linear_memory_t in the C API), as created by
// wasmtime::MemoryCreator::new_memory() (wasmtime_new_memory_callback_t):
//
//  - the memory is page aligned and a multiple of the page size,
//  - it is zero-filled,
//  - byte_size() bytes are accessible, byte_capacity() bytes can be reached by
//    growing without relocating as_ptr(),
//  - the guard region past the capacity is unmapped, so that compiled guest
//    code can elide bounds checks and rely on a trap instead.
//
// The guest memory is assembled out of ordinary allocations, so that a wasm
// guest uses memory that is accounted for, and reclaimable, like the rest of
// the shard's memory. This was verified against the wasmtime 48 C API, running
// a wasm module whose memory is served by a contiguous_mapping.
class wasm_linear_memory {
    // A wasm page, the granularity in which a guest grows its memory.
    static constexpr size_t wasm_page_size = 64 * 1024;
    std::vector<chunk> _chunks;
    size_t _capacity;
    memory::contiguous_mapping _mapping;
public:
    // Arguments as passed to wasmtime_new_memory_callback_t, with zero meaning
    // "unspecified" for maximum and reserved_size.
    wasm_linear_memory(size_t minimum, size_t maximum, size_t reserved_size, size_t guard_size)
            : _capacity(reserved_size ? reserved_size : std::max(minimum, maximum))
            , _mapping(_capacity + guard_size) {
        grow_to(minimum);
    }
    char* as_ptr() const noexcept { return _mapping.data(); }
    size_t byte_size() const noexcept { return _mapping.size(); }
    size_t byte_capacity() const noexcept { return _capacity; }
    void grow_to(size_t new_size) {
        auto ranges = std::vector<std::span<char>>();
        for (auto size = byte_size(); size < new_size; size += wasm_page_size) {
            _chunks.emplace_back(wasm_page_size);
            // wasmtime requires the guest memory to be zero-filled.
            std::memset(_chunks.back().data(), 0, wasm_page_size);
            ranges.push_back(_chunks.back().range());
        }
        _mapping.append(ranges);
    }
};

SEASTAR_THREAD_TEST_CASE(test_usable_as_wasmtime_guest_memory) {
    if (!file_backed_memory_layout()) {
        testlog.info("skipping: the shard's memory is not backed by a file");
        return;
    }
    constexpr size_t wasm_page_size = 64 * 1024;
    // What wasmtime asks for on x86-64 for a guest declaring (memory 4 100).
    constexpr size_t reserved_size = 4ull << 30;
    constexpr size_t guard_size = 32 << 20;
    auto mem = wasm_linear_memory(4 * wasm_page_size, 100 * wasm_page_size, reserved_size, guard_size);
    BOOST_REQUIRE_EQUAL(mem.byte_size(), 4 * wasm_page_size);
    BOOST_REQUIRE_EQUAL(mem.byte_capacity(), reserved_size);
    BOOST_REQUIRE_EQUAL(reinterpret_cast<uintptr_t>(mem.as_ptr()) % memory::page_size, 0u);
    auto guest = std::span(mem.as_ptr(), mem.byte_size());
    BOOST_REQUIRE(std::all_of(guest.begin(), guest.end(), [] (char c) { return c == 0; }));
    // The guest stores across the seams between the allocations backing it.
    for (size_t i = 0; i < mem.byte_size(); ++i) {
        guest[i] = char(i);
    }
    // memory.grow keeps the base pointer, so compiled code needs no fixups.
    auto base = mem.as_ptr();
    mem.grow_to(100 * wasm_page_size);
    BOOST_REQUIRE(mem.as_ptr() == base);
    BOOST_REQUIRE_EQUAL(mem.byte_size(), 100 * wasm_page_size);
    for (size_t i = 0; i < 4 * wasm_page_size; ++i) {
        BOOST_REQUIRE_EQUAL(guest[i], char(i));
    }
    auto grown = std::span(mem.as_ptr() + 4 * wasm_page_size, 96 * wasm_page_size);
    BOOST_REQUIRE(std::all_of(grown.begin(), grown.end(), [] (char c) { return c == 0; }));
    // Everything from byte_size() to the end of the guard region traps, which is
    // what lets compiled guest code skip bounds checks.
    BOOST_REQUIRE(!readable(mem.as_ptr() + mem.byte_size()));
    BOOST_REQUIRE(!readable(mem.as_ptr() + mem.byte_capacity()));
    BOOST_REQUIRE(!readable(mem.as_ptr() + mem.byte_capacity() + guard_size - 1));
}
