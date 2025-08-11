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
 * Copyright (C) 2023 ScyllaDB
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <functional>

#include <linux/perf_event.h>
#endif

#include <seastar/core/posix.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/util/modules.hh>

namespace seastar {
namespace internal {

struct timer_cfg {
    int signal_number;
};

class posix_timer {
    timer_t _timer;
public:
    explicit posix_timer(timer_cfg cfg, clockid_t clock_id = CLOCK_THREAD_CPUTIME_ID);
    virtual ~posix_timer();
    void arm_timer(std::chrono::nanoseconds);
    void disarm_timer();
};

class linux_perf_event {
    file_desc _fd;
    bool _enabled = false;
    uint64_t _current_period = 0;
    struct ::perf_event_mmap_page* _mmap;
    char* _data_area;
    size_t _data_area_mask;
    // after the detector has been armed (i.e., _enabled is true), this
    // is the moment at or after which the next signal is expected to occur
    // and can be used for detecting spurious signals
    sched_clock::time_point _next_signal_time{};
private:
    class data_area_reader {
        std::reference_wrapper<linux_perf_event> _p;
        const char* _data_area;
        size_t _data_area_mask;
        uint64_t _head;
        uint64_t _tail;
    public:
        explicit data_area_reader(linux_perf_event& p)
                : _p(p)
                , _data_area(p._data_area)
                , _data_area_mask(p._data_area_mask) {
            _head = _p.get()._mmap->data_head;
            _tail = _p.get()._mmap->data_tail;
            std::atomic_thread_fence(std::memory_order_acquire); // required after reading data_head
        }
        data_area_reader(data_area_reader&& o) 
                : _p(o._p)
                , _data_area(o._data_area)
                , _data_area_mask(o._data_area_mask)
                , _head(o._head)
                , _tail(o._tail) {
            o._data_area = nullptr;
        }
        ~data_area_reader() {
            if(_data_area != nullptr) {
                std::atomic_thread_fence(std::memory_order_release); // not documented, but probably required before writing data_tail
                _p.get()._mmap->data_tail = _tail;
            }
        }
        uint64_t read_u64() {

            uint64_t ret;
            // We cannot wrap around if the 8-byte unit is aligned
            std::copy_n(_data_area + (_tail & _data_area_mask), 8, reinterpret_cast<char*>(&ret));
            _tail += 8;
            return ret;
        }
        template <typename S>
        S read_struct() {
            static_assert(sizeof(S) % 8 == 0);
            S ret;
            char* p = reinterpret_cast<char*>(&ret);
            for (size_t i = 0; i != sizeof(S); i += 8) {
                uint64_t w = read_u64();
                std::copy_n(reinterpret_cast<const char*>(&w), 8, p + i);
            }
            return ret;
        }
        void skip(uint64_t bytes_to_skip) {
            _tail += bytes_to_skip;
        }
        // skip all the remaining data in the buffer, as-if calling read until
        // have_data returns false (but much faster)
        void skip_all() {
            _tail = _head;
        }
        bool have_data() const {
            return _head != _tail;
        }
    };

    explicit linux_perf_event(file_desc fd);
public:

    class kernel_backtrace {
        data_area_reader _reader;
    public:
        kernel_backtrace(data_area_reader reader) : _reader(std::move(reader)) {}
        void read_backtrace(std::function<void(uintptr_t)>);
    };

    linux_perf_event(linux_perf_event&&);
    static linux_perf_event try_make(timer_cfg cfg);
    ~linux_perf_event();
    void arm_timer(std::chrono::nanoseconds);
    void disarm_timer();
    bool is_spurious_signal();
    std::optional<kernel_backtrace> try_get_kernel_backtrace();
};

}
}
