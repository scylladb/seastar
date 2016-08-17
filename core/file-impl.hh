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

#include "file.hh"
#include <deque>

class posix_file_impl : public file_impl {
public:
    int _fd;
    posix_file_impl(int fd, file_open_options options);
    virtual ~posix_file_impl() override;
    future<size_t> write_dma(uint64_t pos, const void* buffer, size_t len, const io_priority_class& pc);
    future<size_t> write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc);
    future<size_t> read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& pc);
    future<size_t> read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc);
    future<> flush(void);
    future<struct stat> stat(void);
    future<> truncate(uint64_t length);
    future<> discard(uint64_t offset, uint64_t length);
    virtual future<> allocate(uint64_t position, uint64_t length) override;
    future<uint64_t> size();
    virtual future<> close() noexcept override;
    virtual subscription<directory_entry> list_directory(std::function<future<> (directory_entry de)> next) override;
private:
    void query_dma_alignment();
};

// The Linux XFS implementation is challenged wrt. append: a write that changes
// eof will be blocked by any other concurrent AIO operation to the same file, whether
// it changes file size or not. Furthermore, ftruncate() will also block and be blocked
// by AIO, so attempts to game the system and call ftruncate() have to be done very carefully.
//
// Other Linux filesystems may have different locking rules, so this may need to be
// adjusted for them.
class append_challenged_posix_file_impl : public posix_file_impl {
    // File size as a result of completed kernel operations (writes and truncates)
    uint64_t _committed_size;
    // File size as a result of seastar API calls
    uint64_t _logical_size;
    // Pending operations
    enum class opcode {
        invalid,
        read,
        write,
        truncate,
        flush,
    };
    struct op {
        opcode type;
        uint64_t pos;
        size_t len;
        std::function<future<> ()> run;
    };
    // Queue of pending operations; processed from front to end to avoid
    // starvation, but can issue concurrent operations.
    std::deque<op> _q;
    unsigned _max_size_changing_ops = 0;
    unsigned _current_non_size_changing_ops = 0;
    unsigned _current_size_changing_ops = 0;
    // Set when the user closes the file
    bool _done = false;
    bool _sloppy_size = false;
    // Fulfiled when _done and I/O is complete
    promise<> _completed;
private:
    void commit_size(uint64_t size);
    bool size_changing(const op& candidate) const;
    bool may_dispatch(const op& candidate) const;
    void dispatch(op& candidate);
    void optimize_queue();
    void process_queue();
    bool may_quit() const;
    void enqueue(op&& op);
public:
    append_challenged_posix_file_impl(int fd, file_open_options options, unsigned max_size_changing_ops);
    ~append_challenged_posix_file_impl() override;
    future<size_t> read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& pc) override;
    future<size_t> read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) override;
    future<size_t> write_dma(uint64_t pos, const void* buffer, size_t len, const io_priority_class& pc) override;
    future<size_t> write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) override;
    future<> flush() override;
    future<struct stat> stat() override;
    future<> truncate(uint64_t length) override;
    future<uint64_t> size() override;
    future<> close() noexcept override;
};

class blockdev_file_impl : public posix_file_impl {
public:
    blockdev_file_impl(int fd, file_open_options options);
    future<> truncate(uint64_t length) override;
    future<> discard(uint64_t offset, uint64_t length) override;
    future<uint64_t> size() override;
    virtual future<> allocate(uint64_t position, uint64_t length) override;
};

