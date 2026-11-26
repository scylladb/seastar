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
 * Copyright (C) 2026-present ScyllaDB
 */

// Measures the CPU cost of the file reads that return a buffer:
// dma_read_bulk(), dma_read() and dma_read_exactly().
//
// The "fast" tests read ranges inside the file, where the first read returns
// all the bytes. The "eof" tests read ranges that end past the end of the file,
// so the first read is short and dma_read_bulk() takes its slow path.
//
// The files are created in $TMPDIR (/tmp by default). On tmpfs, a read
// completes without waiting for a device, so the results show the CPU cost of
// the read path. On a filesystem on a disk, the runtime also includes the
// wait for the device.

#include <seastar/testing/perf_tests.hh>

#include <seastar/core/align.hh>
#include <seastar/core/file.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/tmp_file.hh>

#include <cstring>
#include <vector>

using namespace seastar;

class file_read {
protected:
    static constexpr uint64_t file_size = 1 << 20;
    // The size of the second file is not a multiple of the disk alignment.
    static constexpr uint64_t unaligned_tail = 1000;

    tmp_dir _dir;
    file _aligned;
    file _unaligned;
    uint64_t _next = 0;

    file make_file(const char* name, uint64_t size) {
        auto f = open_file_dma((_dir.get_path() / name).native(), open_flags::rw | open_flags::create).get();
        const auto aligned_size = align_up<uint64_t>(size, f.disk_write_dma_alignment());
        auto buf = temporary_buffer<char>::aligned(f.memory_dma_alignment(), aligned_size);
        std::memset(buf.get_write(), 'x', buf.size());
        f.dma_write(0, buf.get(), buf.size()).get();
        f.truncate(size).get();
        f.flush().get();
        return f;
    }

    // Returns the offset of the next block of `size` bytes in the file, so
    // that the reads go through the whole file.
    uint64_t next_offset(size_t size) noexcept {
        const auto offset = _next;
        _next = (_next + size) % file_size;
        return offset;
    }

public:
    file_read() {
        _dir.create().get();
        _aligned = make_file("aligned", file_size);
        _unaligned = make_file("unaligned", file_size + unaligned_tail);
    }

    ~file_read() {
        _aligned.close().get();
        _unaligned.close().get();
        _dir.remove().get();
    }
};

// Reads an aligned 4 KiB block.
PERF_TEST_F(file_read, bulk_4k) {
    return _aligned.dma_read_bulk<char>(next_offset(4096), 4096).then([] (temporary_buffer<char> buf) {
        perf_tests::do_not_optimize(buf);
    });
}

// Reads 300 bytes that start 100 bytes into a block, as a read of a small
// record does.
PERF_TEST_F(file_read, bulk_300_unaligned) {
    return _aligned.dma_read_bulk<char>(next_offset(4096) + 100, 300).then([] (temporary_buffer<char> buf) {
        perf_tests::do_not_optimize(buf);
    });
}

// Reads an aligned 128 KiB range.
PERF_TEST_F(file_read, bulk_128k) {
    return _aligned.dma_read_bulk<char>(next_offset(128 * 1024), 128 * 1024).then([] (temporary_buffer<char> buf) {
        perf_tests::do_not_optimize(buf);
    });
}

// Reads 16 aligned 4 KiB blocks in parallel, and reports the cost of one read.
PERF_TEST_F(file_read, bulk_4k_x16) {
    constexpr size_t parallel_reads = 16;
    std::vector<future<temporary_buffer<char>>> reads;
    reads.reserve(parallel_reads);
    for (size_t i = 0; i < parallel_reads; ++i) {
        reads.push_back(_aligned.dma_read_bulk<char>(next_offset(4096), 4096));
    }
    return when_all_succeed(reads.begin(), reads.end()).then([] (std::vector<temporary_buffer<char>> bufs) {
        perf_tests::do_not_optimize(bufs);
        return parallel_reads;
    });
}

// Reads 4 KiB with dma_read(), which trims the result to the requested size.
PERF_TEST_F(file_read, read_4k) {
    return _aligned.dma_read<char>(next_offset(4096), 4096).then([] (temporary_buffer<char> buf) {
        perf_tests::do_not_optimize(buf);
    });
}

// Reads 4 KiB with dma_read_exactly(), as a page cache of a file does.
PERF_TEST_F(file_read, exactly_4k) {
    return _aligned.dma_read_exactly<char>(next_offset(4096), 4096).then([] (temporary_buffer<char> buf) {
        perf_tests::do_not_optimize(buf);
    });
}

// Reads a range that starts 512 bytes before the end of a file whose size is
// aligned. The first read stops at the end of the file, at an aligned
// position, so dma_read_bulk() reads again there and gets 0 bytes.
PERF_TEST_F(file_read, bulk_eof_aligned) {
    return _aligned.dma_read_bulk<char>(file_size - 512, 4096).then([] (temporary_buffer<char> buf) {
        perf_tests::do_not_optimize(buf);
    });
}

// Reads a range that starts 500 bytes before the end of a file whose size is
// not aligned. The first read stops at the end of the file, at a position that
// is not aligned.
PERF_TEST_F(file_read, bulk_eof_unaligned) {
    return _unaligned.dma_read_bulk<char>(file_size + unaligned_tail - 500, 4096).then([] (temporary_buffer<char> buf) {
        perf_tests::do_not_optimize(buf);
    });
}
