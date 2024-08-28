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
 * Copyright (C) 2024 ScyllaDB.
 */


#pragma once
#include <cstddef>
#include <seastar/core/file.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/byteorder.hh>

namespace seastar {
namespace internal {

enum class trace_event {
    OPENING,
    BUFFER_HEAD,
    T800,
    TICK,
};

size_t tick_event_size();

template <trace_event E>
struct event_tracer {
    static int size() noexcept;
    template <typename... Args>
    static void put(char* buf, Args&&... args) noexcept;
};

class tracer {
public:
    static constexpr int buffer_size = 128*1024;
    using opening_fn = noncopyable_function<size_t(char*)>;

private:
    using clock_t = std::chrono::steady_clock;

    class impl {
        static constexpr int max_pool_size = 8;

        tracer& _owner;
        clock_t::time_point _prev_ts;
        char* _head;
        char* _tail;
        unsigned int _skipped_events;
        temporary_buffer<char> _current;
        circular_buffer<temporary_buffer<char>> _pool;

        file _out;
        future<> _flush_fiber = make_ready_future<>();
        circular_buffer<temporary_buffer<char>> _flush_queue;
        condition_variable _flush_signal;
        size_t _flush_pos;

        timer<lowres_clock> _timeout;
        size_t _total_size;
        const size_t _max_size;

        timer<lowres_clock> _ticker;

        future<> run_flush_fiber() noexcept;
        void flush() noexcept;
        void set_current(temporary_buffer<char>) noexcept;

        template <trace_event Ev>
        void put(char* buf) noexcept {
            *(uint8_t*)buf = static_cast<uint8_t>(Ev);
        }

        std::chrono::microseconds fetch_add_ts() noexcept {
            auto now = clock_t::now();
            auto delta = std::chrono::duration_cast<std::chrono::microseconds>(now - _prev_ts);
            _prev_ts = now;
            return delta;
        }

    public:
        impl(tracer&, std::chrono::seconds timeout, size_t max_size, file out, opening_fn);

        template <trace_event E, typename... Args>
        void trace(Args&&... args) noexcept {
            if (_head == nullptr) [[unlikely]] {
                _skipped_events++;
                return;
            }

            size_t hs = sizeof(uint8_t) + sizeof(uint16_t);
            size_t s = event_tracer<E>::size() + hs;
            if (_head + s > _tail) [[unlikely]] {
                flush();
            }
            put<E>(_head);
            write_le(_head + sizeof(uint8_t), (uint16_t)fetch_add_ts().count());
            event_tracer<E>::put(_head + hs, std::forward<Args>(args)...);
            _head += s;
        }

        future<> stop();
    };

    std::unique_ptr<impl> _impl;

    void disable() noexcept;

public:
    template <trace_event E, typename... Args>
    void trace(Args&&... args) noexcept {
        if (_impl) [[unlikely]] {
            _impl->trace<E>(std::forward<Args>(args)...);
        }
    }

    void enable(std::chrono::seconds timeout, size_t max_size, file out, opening_fn);
};

} // internal namespace
} // seastar namespace
