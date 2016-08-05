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
    future<size_t> size(void);
    virtual future<> close() noexcept override;
    virtual subscription<directory_entry> list_directory(std::function<future<> (directory_entry de)> next) override;
private:
    void query_dma_alignment();
};

class blockdev_file_impl : public posix_file_impl {
public:
    blockdev_file_impl(int fd, file_open_options options);
    future<> truncate(uint64_t length) override;
    future<> discard(uint64_t offset, uint64_t length) override;
    future<size_t> size(void) override;
    virtual future<> allocate(uint64_t position, uint64_t length) override;
};

