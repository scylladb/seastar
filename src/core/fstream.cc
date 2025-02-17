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

#ifdef SEASTAR_MODULE
module;
#endif

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <malloc.h>
#include <string.h>
#include <ratio>
#include <optional>
#include <utility>
#include <seastar/util/assert.hh>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/fstream.hh>
#include <seastar/core/align.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/io_intent.hh>
#endif

namespace seastar {

static_assert(std::is_nothrow_constructible_v<data_source>);
static_assert(std::is_nothrow_move_constructible_v<data_source>);

static_assert(std::is_nothrow_constructible_v<data_sink>);
static_assert(std::is_nothrow_move_constructible_v<data_sink>);

static_assert(std::is_nothrow_constructible_v<temporary_buffer<char>>);
static_assert(std::is_nothrow_move_constructible_v<temporary_buffer<char>>);

static_assert(std::is_nothrow_constructible_v<input_stream<char>>);
static_assert(std::is_nothrow_move_constructible_v<input_stream<char>>);

static_assert(std::is_nothrow_constructible_v<output_stream<char>>);
static_assert(std::is_nothrow_move_constructible_v<output_stream<char>>);

// The buffers size must not be greater than the limit, but when capping
// it we make it 2^n to better utilize the memory allocated for buffers
template <typename T>
static inline T select_buffer_size(T configured_value, T maximum_value) noexcept {
    if (configured_value <= maximum_value) {
        return configured_value;
    } else {
        return T(1) << log2floor(maximum_value);
    }
}

template <typename Options>
inline internal::maybe_priority_class_ref get_io_priority(const Options& opts) {
    return internal::maybe_priority_class_ref{};
}

class file_data_source_impl : public data_source_impl {
    struct issued_read {
        uint64_t _pos;
        uint64_t _size;
        future<temporary_buffer<char>> _ready;

        issued_read(uint64_t pos, uint64_t size, future<temporary_buffer<char>> f)
            : _pos(pos), _size(size), _ready(std::move(f)) { }
    };

    reactor& _reactor = engine();
    file _file;
    file_input_stream_options _options;
    uint64_t _pos;
    uint64_t _remain;
    circular_buffer<issued_read> _read_buffers;
    unsigned _reads_in_progress = 0;
    unsigned _current_read_ahead;
    future<> _dropped_reads = make_ready_future<>();
    std::optional<promise<>> _done;
    size_t _current_buffer_size;
    bool _in_slow_start = false;
    io_intent _intent;
    using unused_ratio_target = std::ratio<25, 100>;
private:
    size_t minimal_buffer_size() const {
        return std::min(std::max(_options.buffer_size / 4, size_t(8192)), _options.buffer_size);
    }

    void try_increase_read_ahead() {
        // Read-ahead can be increased up to user-specified limit if the
        // consumer has to wait for a buffer and we are not in a slow start
        // phase.
        if (_current_read_ahead < _options.read_ahead && !_in_slow_start) {
            _current_read_ahead++;
            if (_options.dynamic_adjustments) {
                auto& h = *_options.dynamic_adjustments;
                h.read_ahead = std::max(h.read_ahead, _current_read_ahead);
            }
        }
    }
    unsigned get_initial_read_ahead() const {
        return _options.dynamic_adjustments
               ? std::min(_options.dynamic_adjustments->read_ahead, _options.read_ahead)
               : !!_options.read_ahead;
    }

    void update_history(uint64_t unused, uint64_t total) {
        // We are maintaining two windows each no larger than window_size.
        // Dynamic adjustment logic uses data from both of them, which
        // essentially means that the actual window size is variable and
        // in the range [window_size, 2*window_size].
        auto& h = *_options.dynamic_adjustments;
        h.current_window.total_read += total;
        h.current_window.unused_read += unused;
        if (h.current_window.total_read >= h.window_size) {
            h.previous_window = h.current_window;
            h.current_window = { };
        }
    }
    static bool below_target(uint64_t unused, uint64_t total) {
        return unused * unused_ratio_target::den < total * unused_ratio_target::num;
    }
    void update_history_consumed(uint64_t bytes) {
        if (!_options.dynamic_adjustments) {
            return;
        }
        update_history(0, bytes);
        if (!_in_slow_start) {
            return;
        }
        unsigned new_size = std::min(_current_buffer_size * 2, _options.buffer_size);
        auto& h = *_options.dynamic_adjustments;
        auto total = h.current_window.total_read + h.previous_window.total_read + new_size;
        auto unused = h.current_window.unused_read + h.previous_window.unused_read + new_size;
        // Check whether we can safely increase the buffer size to new_size
        // and still be below unused_ratio_target even if it is entirely
        // dropped.
        if (below_target(unused, total)) {
            _current_buffer_size = new_size;
            _in_slow_start = _current_buffer_size < _options.buffer_size;
        }
    }

    using after_skip = bool_class<class after_skip_tag>;
    void set_new_buffer_size(after_skip skip) {
        if (!_options.dynamic_adjustments) {
            return;
        }
        auto& h = *_options.dynamic_adjustments;
        int64_t total = h.current_window.total_read + h.previous_window.total_read;
        int64_t unused = h.current_window.unused_read + h.previous_window.unused_read;
        if (skip == after_skip::yes && below_target(unused, total)) {
            // Do not attempt to shrink buffer size if we are still below the
            // target. Otherwise, we could get a bad interaction with
            // update_history_consumed() which tries to increase the buffer
            // size as much as possible so that after a single drop we are
            // still below the target.
            return;
        }
        // Calculate the maximum buffer size that would guarantee that we are
        // still below unused_ratio_target even if the subsequent reads are
        // dropped. If it is larger than or equal to the current buffer size do
        // nothing. If it is smaller then we are back in the slow start phase.
        auto new_target = (unused_ratio_target::num * total - unused_ratio_target::den * unused) / (unused_ratio_target::den - unused_ratio_target::num);
        uint64_t new_size = std::max(new_target, int64_t(minimal_buffer_size()));
        new_size = std::max(uint64_t(1) << log2floor(new_size), uint64_t(minimal_buffer_size()));
        if (new_size >= _current_buffer_size) {
            return;
        }
        _in_slow_start = true;
        _current_read_ahead = std::min(_current_read_ahead, 1u);
        _current_buffer_size = new_size;
    }
    void update_history_unused(uint64_t bytes) {
        if (!_options.dynamic_adjustments) {
            return;
        }
        update_history(bytes, bytes);
        set_new_buffer_size(after_skip::yes);
    }
    // Safely ignores read future even if it is not resolved yet.
    void ignore_read_future(future<temporary_buffer<char>> read_future) {
        if (read_future.available()) {
            read_future.ignore_ready_future();
            return;
        }
        auto f = read_future.then_wrapped([] (auto f) { f.ignore_ready_future(); });
        _dropped_reads = _dropped_reads.then([f = std::move(f)] () mutable { return std::move(f); });
    }
public:
    file_data_source_impl(file f, uint64_t offset, uint64_t len, file_input_stream_options options)
            : _file(std::move(f)), _options(options), _pos(offset), _remain(len), _current_read_ahead(get_initial_read_ahead())
    {
        _options.buffer_size = select_buffer_size(_options.buffer_size, _file.disk_read_max_length());
        _current_buffer_size = _options.buffer_size;
        // prevent wraparounds
        set_new_buffer_size(after_skip::no);
        _remain = std::min(std::numeric_limits<uint64_t>::max() - _pos, _remain);
    }
    virtual ~file_data_source_impl() override {
        // If the data source hasn't been closed, we risk having reads in progress
        // that will try to access freed memory.
        SEASTAR_ASSERT(_reads_in_progress == 0);
    }
    virtual future<temporary_buffer<char>> get() override {
        if (!_read_buffers.empty() && !_read_buffers.front()._ready.available()) {
            try_increase_read_ahead();
        }
        issue_read_aheads(1);
        auto ret = std::move(_read_buffers.front());
        _read_buffers.pop_front();
        update_history_consumed(ret._size);
        _reactor._io_stats.fstream_reads += 1;
        _reactor._io_stats.fstream_read_bytes += ret._size;
        if (!ret._ready.available()) {
            _reactor._io_stats.fstream_reads_blocked += 1;
            _reactor._io_stats.fstream_read_bytes_blocked += ret._size;
        }
        return std::move(ret._ready);
    }
    virtual future<temporary_buffer<char>> skip(uint64_t n) override {
        uint64_t dropped = 0;
        while (n) {
            if (_read_buffers.empty()) {
                SEASTAR_ASSERT(n <= _remain);
                _pos += n;
                _remain -= n;
                break;
            }
            auto& front = _read_buffers.front();
            if (n < front._size) {
                front._size -= n;
                front._pos += n;
                front._ready = front._ready.then([n] (temporary_buffer<char> buf) {
                    buf.trim_front(n);
                    return buf;
                });
                break;
            } else {
                ignore_read_future(std::move(front._ready));
                n -= front._size;
                dropped += front._size;
                _reactor._io_stats.fstream_read_aheads_discarded += 1;
                _reactor._io_stats.fstream_read_ahead_discarded_bytes += front._size;
                _read_buffers.pop_front();
            }
        }
        update_history_unused(dropped);
        return make_ready_future<temporary_buffer<char>>();
    }
    virtual future<> close() override {
        _done.emplace();
        if (!_reads_in_progress) {
            _done->set_value();
        }
        _intent.cancel();
        return _done->get_future().then([this] {
            uint64_t dropped = 0;
            for (auto&& c : _read_buffers) {
                _reactor._io_stats.fstream_read_aheads_discarded += 1;
                _reactor._io_stats.fstream_read_ahead_discarded_bytes += c._size;
                dropped += c._size;
                ignore_read_future(std::move(c._ready));
            }
            update_history_unused(dropped);
            return std::move(_dropped_reads);
        });
    }
private:
    void issue_read_aheads(unsigned additional = 0) {
        if (_done) {
            return;
        }
        auto ra = _current_read_ahead + additional;
        _read_buffers.reserve(ra); // prevent push_back() failure
        while (_read_buffers.size() < ra) {
            if (!_remain) {
                if (_read_buffers.size() >= additional) {
                    return;
                }
                _read_buffers.emplace_back(_pos, 0, make_ready_future<temporary_buffer<char>>());
                continue;
            }
            ++_reads_in_progress;
            // if _pos is not dma-aligned, we'll get a short read.  Account for that.
            // Also avoid reading beyond _remain.
            uint64_t align = _file.disk_read_dma_alignment();
            auto start = align_down(_pos, align);
            auto end = std::min(align_up(start + _current_buffer_size, align), _pos + _remain);
            auto len = end - start;
            auto actual_size = std::min(end - _pos, _remain);
            _read_buffers.emplace_back(_pos, actual_size, futurize_invoke([&] {
                    return _file.dma_read_bulk_impl(start, len, get_io_priority(_options), &_intent);
            }).then_wrapped(
                    [this, start, pos = _pos, remain = _remain] (future<temporary_buffer<uint8_t>> ret) {
                --_reads_in_progress;
                if (_done && !_reads_in_progress) {
                    _done->set_value();
                }
                if (ret.failed()) {
                    // no games needed
                    return make_exception_future<temporary_buffer<char>>(ret.get_exception());
                } else {
                    // first or last buffer, need trimming
                    auto tmp = ret.get();
                    auto real_end = start + tmp.size();
                    if (real_end <= pos) {
                        return make_ready_future<temporary_buffer<char>>();
                    }
                    if (real_end > pos + remain) {
                        tmp.trim(pos + remain - start);
                    }
                    if (start < pos) {
                        tmp.trim_front(pos - start);
                    }
                    return make_ready_future<temporary_buffer<char>>(temporary_buffer<char>(reinterpret_cast<char*>(tmp.get_write()), tmp.size(), tmp.release()));
                }
            }));
            _remain -= end - _pos;
            _pos = end;
        };
    }
};

class file_data_source : public data_source {
public:
    file_data_source(file f, uint64_t offset, uint64_t len, file_input_stream_options options)
        : data_source(std::make_unique<file_data_source_impl>(
                std::move(f), offset, len, options)) {}
};


input_stream<char> make_file_input_stream(
        file f, uint64_t offset, uint64_t len, file_input_stream_options options) {
    return input_stream<char>(file_data_source(std::move(f), offset, len, std::move(options)));
}

input_stream<char> make_file_input_stream(
        file f, uint64_t offset, file_input_stream_options options) {
    return make_file_input_stream(std::move(f), offset, std::numeric_limits<uint64_t>::max(), std::move(options));
}

input_stream<char> make_file_input_stream(
        file f, file_input_stream_options options) {
    return make_file_input_stream(std::move(f), 0, std::move(options));
}


class file_data_sink_impl : public data_sink_impl {
    file _file;
    file_output_stream_options _options;
    uint64_t _pos = 0;
    semaphore _write_behind_sem = { _options.write_behind };
    future<> _background_writes_done = make_ready_future<>();
    bool _failed = false;
public:
    file_data_sink_impl(file f, file_output_stream_options options)
            : _file(std::move(f)), _options(options) {
        _options.buffer_size = select_buffer_size<unsigned>(_options.buffer_size, _file.disk_write_max_length());
        _write_behind_sem.ensure_space_for_waiters(1); // So that wait() doesn't throw
    }
    future<> put(net::packet data) override { abort(); }
    virtual temporary_buffer<char> allocate_buffer(size_t size) override {
        return temporary_buffer<char>::aligned(_file.memory_dma_alignment(), size);
    }
    using data_sink_impl::put;
    virtual future<> put(temporary_buffer<char> buf) override {
        uint64_t pos = _pos;
        _pos += buf.size();
        if (!_options.write_behind) {
            return do_put(pos, std::move(buf));
        }
        // Write behind strategy:
        //
        // 1. Issue N writes in parallel, using a semaphore to limit to N
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
    future<> do_put(uint64_t pos, temporary_buffer<char> buf) noexcept {
      try {
        // put() must usually be of chunks multiple of file::dma_alignment.
        // Only the last part can have an unaligned length. If put() was
        // called again with an unaligned pos, we have a bug in the caller.
        SEASTAR_ASSERT(!(pos & (_file.disk_write_dma_alignment() - 1)));
        bool truncate = false;
        auto p = static_cast<const char*>(buf.get());
        size_t buf_size = buf.size();

        if ((buf.size() & (_file.disk_write_dma_alignment() - 1)) != 0) {
            // If buf size isn't aligned, copy its content into a new aligned buf.
            // This should only happen when the user calls output_stream::flush().
            auto tmp = allocate_buffer(align_up(buf.size(), _file.disk_write_dma_alignment()));
            ::memcpy(tmp.get_write(), buf.get(), buf.size());
            ::memset(tmp.get_write() + buf.size(), 0, tmp.size() - buf.size());
            buf = std::move(tmp);
            p = buf.get();
            buf_size = buf.size();
            truncate = true;
        }

        return _file.dma_write_impl(pos, reinterpret_cast<const uint8_t*>(p), buf_size, get_io_priority(_options), nullptr).then(
                [this, pos, buf = std::move(buf), truncate, buf_size] (size_t size) mutable {
            // short write handling
            if (size < buf_size) {
                buf.trim_front(size);
                return do_put(pos + size, std::move(buf)).then([this, truncate] {
                    if (truncate) {
                        return _file.truncate(_pos);
                    }
                    return make_ready_future<>();
                });
            }
            if (truncate) {
                return _file.truncate(_pos);
            }
            return make_ready_future<>();
        });
      } catch (...) {
          return make_exception_future<>(std::current_exception());
      }
    }
    future<> wait() noexcept {
        // restore to pristine state; for flush() + close() sequence
        // (we allow either flush, or close, or both)
        return _write_behind_sem.wait(_options.write_behind).then([this] {
            return std::exchange(_background_writes_done, make_ready_future<>());
        }).finally([this] {
            _write_behind_sem.signal(_options.write_behind);
        });
    }
public:
    virtual future<> flush() override {
        return wait().then([this] {
            return _file.flush();
        });
    }
    virtual future<> close() noexcept override {
        return wait().finally([this] {
            return _file.close();
        });
    }
    virtual size_t buffer_size() const noexcept override { return _options.buffer_size; }
};

future<data_sink> make_file_data_sink(file f, file_output_stream_options options) noexcept {
    try {
        return make_ready_future<data_sink>(std::make_unique<file_data_sink_impl>(f, options));
    } catch (...) {
        return f.close().then_wrapped([ex = std::current_exception(), f] (future<> fut) mutable {
            if (fut.failed()) {
                try {
                    std::rethrow_exception(std::move(ex));
                } catch (...) {
                    std::throw_with_nested(std::runtime_error(fmt::format("While handling failed construction of data_sink, caught exception: {}",
                                fut.get_exception())));
                }
            }
            return make_exception_future<data_sink>(std::move(ex));
        });
    }
}

future<output_stream<char>> make_file_output_stream(file f, size_t buffer_size) noexcept {
    file_output_stream_options options;
    options.buffer_size = buffer_size;
    return make_file_output_stream(std::move(f), options);
}

future<output_stream<char>> make_file_output_stream(file f, file_output_stream_options options) noexcept {
    return make_file_data_sink(std::move(f), options).then([] (data_sink&& ds) {
        return output_stream<char>(std::move(ds));
    });
}

/*
 * template initialization, definition in iostream-impl.hh
 */
template struct internal::stream_copy_consumer<char>;
template future<> copy<char>(input_stream<char>&, output_stream<char>&);

}

