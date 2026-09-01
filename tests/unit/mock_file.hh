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
 * Copyright (C) 2017 ScyllaDB Ltd.
 */

#pragma once

#include <deque>
#include <numeric>
#include <string_view>
#include <vector>

#include <seastar/testing/seastar_test.hh>
#include <seastar/core/file.hh>
#include <seastar/util/assert.hh>

namespace seastar {

class mock_read_only_file final : public file_impl {
    bool _closed = false;
    uint64_t _total_file_size;
    size_t _allowed_read_requests = 0;
    std::function<void(size_t)> _verify_length;
private:
    size_t verify_read(uint64_t position, size_t length) {
        BOOST_CHECK(!_closed);
        BOOST_CHECK_LE(position, _total_file_size);
        BOOST_CHECK_LE(position + length, _total_file_size);
        if (position + length != _total_file_size) {
            _verify_length(length);
        }
        BOOST_CHECK(_allowed_read_requests);
        SEASTAR_ASSERT(_allowed_read_requests);
        _allowed_read_requests--;
        return length;
    }
public:
    explicit mock_read_only_file(uint64_t file_size) noexcept
        : _total_file_size(file_size)
        , _verify_length([] (auto) { })
    { }

    void set_read_size_verifier(std::function<void(size_t)> fn) {
        _verify_length = fn;
    }
    void set_expected_read_size(size_t expected) {
        _verify_length = [expected] (auto length) {
            BOOST_CHECK_EQUAL(length, expected);
        };
    }
    void set_allowed_read_requests(size_t requests) {
        _allowed_read_requests = requests;
    }

    virtual future<size_t> write_dma(uint64_t, const void*, size_t, io_intent*) noexcept override {
        return make_exception_future<size_t>(std::bad_function_call());
    }
    virtual future<size_t> write_dma(uint64_t, std::vector<iovec>, io_intent*) noexcept override {
        return make_exception_future<size_t>(std::bad_function_call());
    }
    virtual future<size_t> read_dma(uint64_t pos, void*, size_t len, io_intent*) noexcept override {
        return make_ready_future<size_t>(verify_read(pos, len));
    }
    virtual future<size_t> read_dma(uint64_t pos, std::vector<iovec> iov, io_intent*) noexcept override {
        auto length = std::accumulate(iov.begin(), iov.end(), size_t(0),
                                      [] (size_t len, const iovec& iov) { return len + iov.iov_len; });
        return make_ready_future<size_t>(verify_read(pos, length));
    }
    virtual future<> flush() noexcept override {
        return make_ready_future<>();
    }
    virtual future<struct stat> stat() noexcept override {
        return make_exception_future<struct stat>(std::bad_function_call());
    }
    virtual future<struct stat> statat(std::string_view name, int flags) noexcept override {
        return make_exception_future<struct stat>(std::bad_function_call());
    }
    virtual future<> truncate(uint64_t) noexcept override {
        return make_exception_future<>(std::bad_function_call());
    }
    virtual future<> discard(uint64_t offset, uint64_t length) noexcept override {
        return make_exception_future<>(std::bad_function_call());
    }
    virtual future<> allocate(uint64_t position, uint64_t length) noexcept override {
        return make_exception_future<>(std::bad_function_call());
    }
    virtual future<uint64_t> size() noexcept override {
        return make_ready_future<uint64_t>(_total_file_size);
    }
    virtual future<> close() noexcept override {
        BOOST_CHECK(!_closed);
        _closed = true;
        return make_ready_future<>();
    }
    virtual subscription<directory_entry> list_directory(std::function<future<> (directory_entry de)>) override {
        throw std::bad_function_call();
    }
    virtual future<temporary_buffer<uint8_t>> dma_read_bulk(uint64_t offset, size_t range_size, io_intent*) noexcept override {
        auto length = verify_read(offset, range_size);
        return make_ready_future<temporary_buffer<uint8_t>>(temporary_buffer<uint8_t>(length));
    }
};

// A write-only file keeping its contents in memory, which can be told to
// complete write requests only partially, the way a buffered (non-O_DIRECT)
// write may do. Every request is checked to start at a DMA-aligned position,
// which is what a writer using dma_write() has to guarantee.
class mock_write_only_file final : public file_impl {
    std::vector<char> _data;
    std::deque<size_t> _partial_writes;
    bool _closed = false;
public:
    // The next write requests complete only the given number of bytes each.
    // Once the list is exhausted, writes complete in full again.
    void complete_partially(std::initializer_list<size_t> lengths) {
        _partial_writes.insert(_partial_writes.end(), lengths.begin(), lengths.end());
    }
    std::string_view contents() const noexcept {
        return {_data.data(), _data.size()};
    }

    virtual future<size_t> write_dma(uint64_t pos, const void* buffer, size_t len, io_intent*) noexcept override {
        BOOST_CHECK(!_closed);
        BOOST_CHECK_EQUAL(pos % _disk_write_dma_alignment, 0u);
        auto written = len;
        if (!_partial_writes.empty()) {
            written = std::min(len, _partial_writes.front());
            _partial_writes.pop_front();
        }
        if (_data.size() < pos + written) {
            _data.resize(pos + written);
        }
        std::copy_n(static_cast<const char*>(buffer), written, _data.begin() + pos);
        return make_ready_future<size_t>(written);
    }
    virtual future<size_t> write_dma(uint64_t, std::vector<iovec>, io_intent*) noexcept override {
        return make_exception_future<size_t>(std::bad_function_call());
    }
    virtual future<size_t> read_dma(uint64_t, void*, size_t, io_intent*) noexcept override {
        return make_exception_future<size_t>(std::bad_function_call());
    }
    virtual future<size_t> read_dma(uint64_t, std::vector<iovec>, io_intent*) noexcept override {
        return make_exception_future<size_t>(std::bad_function_call());
    }
    virtual future<temporary_buffer<uint8_t>> dma_read_bulk(uint64_t, size_t, io_intent*) noexcept override {
        return make_exception_future<temporary_buffer<uint8_t>>(std::bad_function_call());
    }
    virtual future<> flush() noexcept override {
        return make_ready_future<>();
    }
    virtual future<struct stat> stat() noexcept override {
        return make_exception_future<struct stat>(std::bad_function_call());
    }
    virtual future<> truncate(uint64_t length) noexcept override {
        _data.resize(length);
        return make_ready_future<>();
    }
    virtual future<> discard(uint64_t, uint64_t) noexcept override {
        return make_exception_future<>(std::bad_function_call());
    }
    virtual future<> allocate(uint64_t, uint64_t) noexcept override {
        return make_exception_future<>(std::bad_function_call());
    }
    virtual future<uint64_t> size() noexcept override {
        return make_ready_future<uint64_t>(_data.size());
    }
    virtual future<> close() noexcept override {
        BOOST_CHECK(!_closed);
        _closed = true;
        return make_ready_future<>();
    }
    virtual subscription<directory_entry> list_directory(std::function<future<> (directory_entry de)>) override {
        throw std::bad_function_call();
    }
};

}
