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
 * Copyright 2024 ScyllaDB
 */

#ifdef SEASTAR_MODULE
module;
#endif

#include <seastar/core/byteorder.hh>
#include <seastar/util/trace.hh>
#include <seastar/util/log.hh>

namespace seastar {
extern logger seastar_logger;

namespace internal {

template<>
struct event_tracer<trace_event::TICK> {
    static int size() noexcept { return 0; }
    static void put(char* buf) noexcept { }
};

size_t tick_event_size() { return event_tracer<trace_event::TICK>::size(); }

void tracer::impl::set_current(temporary_buffer<char> buf) noexcept {
    _current = std::move(buf);
    _head = _current.get_write();
    _tail = _head + _current.size();

    put<trace_event::BUFFER_HEAD>(_head);
    write_le(_head + 1, (uint32_t)fetch_add_ts().count());
    write_le(_head + 5, (uint32_t)std::exchange(_skipped_events, 0));
    _head += sizeof(uint8_t) + 2 * sizeof(uint32_t);
}

void tracer::impl::flush() noexcept {
    if (_head < _tail) {
        put<trace_event::T800>(_head);
    }
    _flush_queue.push_back(std::move(_current));
    _flush_signal.signal();
    if (_pool.empty()) {
        _head = nullptr;
    } else {
        set_current(std::move(_pool.front()));
        _pool.pop_front();
    }
    _total_size += buffer_size;
    if (_total_size >= _max_size) {
        _owner.disable();
    }
}

future<> tracer::impl::run_flush_fiber() noexcept {
    while (true) {
        if (!_flush_queue.empty()) {
            auto buf = std::move(_flush_queue.front());
            _flush_queue.pop_front();
            co_await _out.dma_write(_flush_pos, buf.get(), buf.size());
            _flush_pos += buf.size();
            if (_head == nullptr) {
                set_current(std::move(buf));
            } else {
                _pool.push_back(std::move(buf));
            }
            continue;
        }
        if (!_timeout.armed()) {
            break;
        }
        co_await _flush_signal.wait();
    }
}

tracer::impl::impl(tracer& owner, std::chrono::seconds timeout, size_t max_size, file out, noncopyable_function<size_t(char*)> opening)
        : _owner(owner)
        , _prev_ts(clock_t::now())
        , _skipped_events(0)
        , _out(std::move(out))
        , _flush_pos(0)
        , _timeout([this] { _owner.disable(); })
        , _total_size(0)
        , _max_size(max_size)
        , _ticker([this] mutable { trace<trace_event::TICK>(); })
{
    _flush_queue.reserve(max_pool_size);

    _pool.reserve(max_pool_size);
    for (int i = 0; i < max_pool_size; i++) {
        auto buf = temporary_buffer<char>::aligned(_out.memory_dma_alignment(), buffer_size);
        _pool.emplace_back(std::move(buf));
    }
    auto buf = temporary_buffer<char>::aligned(_out.memory_dma_alignment(), buffer_size);
    set_current(std::move(buf));

    put<trace_event::OPENING>(_head);
    write_le(_head + 1, (uint8_t)this_shard_id());
    auto size = opening(_head + 4);
    assert(size <= std::numeric_limits<uint16_t>::max());
    write_le(_head + 2, (uint16_t)size);
    _head += sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t) + size;

    _timeout.arm(timeout);
    _flush_fiber = run_flush_fiber();

    _ticker.arm_periodic(std::chrono::milliseconds(32));
}

future<> tracer::impl::stop() {
    _ticker.cancel();
    _timeout.cancel();
    if (_head != nullptr && _head != _current.get()) {
        flush();
    }
    co_await std::move(_flush_fiber);
    assert(_flush_queue.empty());
    co_await _out.close();
    seastar_logger.info("Stopped tracing on shard");
}

void tracer::enable(std::chrono::seconds timeout, size_t max_size, file out, opening_fn opening) {
    if (!_impl) {
        _impl = std::make_unique<impl>(*this, timeout, max_size, std::move(out), std::move(opening));
    }
}

void tracer::disable() noexcept {
    auto ip = std::move(_impl);
    if (ip) {
        auto& i = *ip;
        (void)i.stop().finally([i = std::move(ip)] {});
    }
}

} // internal namespace
} // seastar namespace
