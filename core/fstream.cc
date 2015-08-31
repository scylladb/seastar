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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#include "fstream.hh"
#include "align.hh"
#include "circular_buffer.hh"
#include "semaphore.hh"
#include "reactor.hh"
#include <malloc.h>
#include <string.h>

class file_data_source_impl : public data_source_impl {
    file _file;
    file_input_stream_options _options;
    uint64_t _pos;
    circular_buffer<future<temporary_buffer<char>>> _read_buffers;
    unsigned _reads_in_progress = 0;
    std::experimental::optional<promise<>> _done;
public:
    file_data_source_impl(file f, file_input_stream_options options)
            : _file(std::move(f)), _options(options), _pos(_options.offset) {}
    virtual future<temporary_buffer<char>> get() override {
        if (_read_buffers.empty()) {
            issue_read_aheads(1);
        }
        auto ret = std::move(_read_buffers.front());
        _read_buffers.pop_front();
        return ret;
    }
    virtual future<> close() {
        _done.emplace();
        if (!_reads_in_progress) {
            _done->set_value();
        }
        return _done->get_future().then([this] {
            for (auto&& c : _read_buffers) {
                c.ignore_ready_future();
            }
        });
    }
private:
    void issue_read_aheads(unsigned min_ra = 0) {
        if (_done) {
            return;
        }
        auto ra = std::max(min_ra, _options.read_ahead);
        while (_read_buffers.size() < ra) {
            ++_reads_in_progress;
            // if _pos is not dma-aligned, we'll get a short read.  Account for that.
            auto now = _options.buffer_size - _pos % _options.buffer_size;
            _read_buffers.push_back(_file.dma_read_bulk<char>(_pos, now).then_wrapped(
                    [this] (future<temporary_buffer<char>> ret) {
                issue_read_aheads();
                --_reads_in_progress;
                if (_done && !_reads_in_progress) {
                    _done->set_value();
                }
                return ret;
            }));
            _pos += now;
        };
    }
};

class file_data_source : public data_source {
public:
    file_data_source(file f, file_input_stream_options options)
        : data_source(std::make_unique<file_data_source_impl>(
                std::move(f), options)) {}
};

input_stream<char> make_file_input_stream(
        file f, file_input_stream_options options) {
    return input_stream<char>(file_data_source(std::move(f), options));
}

input_stream<char> make_file_input_stream(
        file f, uint64_t offset, size_t buffer_size) {
    file_input_stream_options options;
    options.offset = offset;
    options.buffer_size = buffer_size;
    return make_file_input_stream(std::move(f), options);
}

class file_data_sink_impl : public data_sink_impl {
    file _file;
    file_output_stream_options _options;
    uint64_t _pos = 0;
    // At offsets < _preallocating_at, preallocation was completed.
    // At offsets >= _preallocating_at, we may be preallocating
    // disk space, so all writes must wait until _preallocation_done[i]
    // becomes ready (in _options.preallocation_size units)
    // At offsets >= _preallocating_at + _options.preallocation_size *
    //               _preallocation_done.size(), we have not yet started.
    uint64_t _preallocating_at = 0;
    // Each promise vector lists writes that are waiting for a single preallocation
    circular_buffer<std::vector<promise<>>> _preallocation_done;
    semaphore _write_behind_sem = { _options.write_behind };
    future<> _background_writes_done = make_ready_future<>();
    bool _failed = false;
public:
    file_data_sink_impl(file f, file_output_stream_options options)
            : _file(std::move(f)), _options(options) {}
    future<> put(net::packet data) { abort(); }
    virtual temporary_buffer<char> allocate_buffer(size_t size) override {
        return temporary_buffer<char>::aligned(file::dma_alignment, size);
    }
    virtual future<> put(temporary_buffer<char> buf) override {
        uint64_t pos = _pos;
        _pos += buf.size();
        if (!_options.write_behind) {
            return do_put(pos, std::move(buf));
        }
        // Write behind strategy:
        //
        // 1. Issue N writes in parallel, using a semphore to limit to N
        // 2. Collect results in _background_writes_done, merging exception futures
        // 3. If we've already seen a failure, don't issue more writes.
        return _write_behind_sem.wait().then([this, pos, buf = std::move(buf)] () mutable {
            if (_failed) {
                _write_behind_sem.signal();
                auto ret = std::move(_background_writes_done);
                _background_writes_done = make_ready_future<>();
                return ret;
            }
            auto this_write_done = do_put(pos, std::move(buf)).finally([this] {
                _write_behind_sem.signal();
            });
            _background_writes_done = when_all(std::move(_background_writes_done), std::move(this_write_done))
                    .then([this] (std::tuple<future<>, future<>> possible_errors) {
                // merge the two errors, preferring the first
                auto& e1 = std::get<0>(possible_errors);
                auto& e2 = std::get<1>(possible_errors);
                if (e1.failed()) {
                    e2.ignore_ready_future();
                    return std::move(e1);
                } else {
                    if (e2.failed()) {
                        _failed = true;
                    }
                    return std::move(e2);
                }
            });
            return make_ready_future<>();
        });
    }
private:
    virtual future<> do_put(uint64_t pos, temporary_buffer<char> buf) {
        // put() must usually be of chunks multiple of file::dma_alignment.
        // Only the last part can have an unaligned length. If put() was
        // called again with an unaligned pos, we have a bug in the caller.
        assert(!(pos & (file::dma_alignment - 1)));
        bool truncate = false;
        auto p = static_cast<const char*>(buf.get());
        size_t buf_size = buf.size();

        if ((buf.size() & (file::dma_alignment - 1)) != 0) {
            // If buf size isn't aligned, copy its content into a new aligned buf.
            // This should only happen when the user calls output_stream::flush().
            auto tmp = allocate_buffer(align_up(buf.size(), file::dma_alignment));
            ::memcpy(tmp.get_write(), buf.get(), buf.size());
            buf = std::move(tmp);
            p = buf.get();
            buf_size = buf.size();
            truncate = true;
        }
        auto prealloc = preallocate(pos, buf.size());
        return prealloc.then([this, pos, p, buf_size, truncate, buf = std::move(buf)] () mutable {
            return _file.dma_write(pos, p, buf_size).then(
                    [this, buf = std::move(buf), truncate] (size_t size) {
                if (truncate) {
                    return _file.truncate(_pos);
                }
                return make_ready_future<>();
            });
        });
    }
    future<> preallocate(uint64_t pos, uint64_t size) {
        auto ret = make_ready_future<>();
    restart:
        if (pos + size <= _preallocating_at) {
            return ret;
        }
        auto skip = std::max(_preallocating_at, pos) - pos;
        pos += skip;
        size -= skip;
        size_t idx = (pos - _preallocating_at) / _options.preallocation_size;
        while (size) {
            if (idx == _preallocation_done.size()) {
                start_new_preallocation();
                // May have caused _preallocating_at to change, so redo the loop
                goto restart;
            }
            _preallocation_done[idx].emplace_back();
            auto this_prealloc_done = _preallocation_done[idx].back().get_future();
            ret = when_all(std::move(ret), std::move(this_prealloc_done)).discard_result();
            skip = std::min<uint64_t>(size, _options.preallocation_size);
            pos += skip;
            size -= skip;
            ++idx;
        }
        return ret;
    }
    void start_new_preallocation() {
        auto pos = _preallocating_at + _preallocation_done.size() * _options.preallocation_size;
        _preallocation_done.emplace_back();
        _file.allocate(pos, _options.preallocation_size).then_wrapped([this, pos] (future<> ret) {
            complete_preallocation(pos, std::move(ret));
        });
    }
    void complete_preallocation(uint64_t pos, future<> result) {
        // Preallocation may have failed.  But it's just preallocation,
        // so we can ignore the result.
        result.ignore_ready_future();
        size_t idx = (pos - _preallocating_at) / _options.preallocation_size;
        for (auto&& pr : _preallocation_done[idx]) {
            pr.set_value();
        }
        _preallocation_done[idx].clear();
        while (!_preallocation_done.empty() && _preallocation_done.front().empty()) {
            _preallocation_done.pop_front();
            _preallocating_at += _options.preallocation_size;
        }
    }
    future<> wait() {
        return _write_behind_sem.wait(_options.write_behind).then([this] {
            return _background_writes_done.then([this] {
                // restore to pristine state; for flush() + close() sequence
                // (we allow either flush, or close, or both)
                _write_behind_sem.signal(_options.write_behind);
                _background_writes_done = make_ready_future<>();
            });
        });
    }
public:
    virtual future<> flush() override {
        return wait().then([this] {
            return _file.flush();
        });
    }
    virtual future<> close() {
        return wait().then([this] {
            return _file.close();
        });
    }
};

class file_data_sink : public data_sink {
public:
    file_data_sink(file f, file_output_stream_options options)
        : data_sink(std::make_unique<file_data_sink_impl>(
                std::move(f), options)) {}
};

output_stream<char> make_file_output_stream(file f, size_t buffer_size) {
    file_output_stream_options options;
    options.buffer_size = buffer_size;
    return make_file_output_stream(std::move(f), options);
}

output_stream<char> make_file_output_stream(file f, file_output_stream_options options) {
    return output_stream<char>(file_data_sink(std::move(f), options), options.buffer_size, true);
}

