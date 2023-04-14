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

#pragma once

#include <seastar/core/scheduling.hh>
#include <seastar/util/backtrace.hh>

#ifndef SEASTAR_MODULE
#include <utility>
#endif

namespace seastar {

SEASTAR_MODULE_EXPORT

inline int64_t rdtsc() {
    uint64_t rax, rdx;
    asm volatile ( "rdtsc" : "=a" (rax), "=d" (rdx) );
    return (int64_t)(( rdx << 32 ) + rax);
}

struct tracer {
    struct entry {
        uint64_t event;
        uint64_t id;
        uint64_t arg;
        int64_t ts;
    };

    tracer();
    static constexpr size_t buffer_size = (32 * 1024);
    std::vector<entry> _buf;
    size_t _head = 0;
    size_t _tail = buffer_size - 1;
    void add(uint64_t event, uint64_t id, uint64_t arg) {
        if (_head == _tail) {
            return;
        }
        _buf[_head++] = entry{.event = event, .id = id, .arg = arg, .ts = rdtsc()};
        if (_head % (buffer_size / 2) == 0) {
            commit();
            if (_head == buffer_size) {
                _head = 0;
            }
        }
    }

    struct impl;
    std::unique_ptr<impl> _impl;
    void commit();
    void start();
    future<> stop();
};
extern thread_local tracer g_tracer;

extern thread_local uint64_t fresh_task_id;
extern thread_local uint64_t current_task_id;

struct task_id {
    uint64_t _value;
    task_id(uint64_t id = current_task_id) : _value(id) {}
    operator uint64_t() { return _value; };
};

struct [[nodiscard]] switch_task {
    task_id _prev;
    switch_task(uint64_t event, uint64_t id) {
        current_task_id = id;
        g_tracer.add(event, _prev, current_task_id);
    }
    ~switch_task() {
        current_task_id = _prev;
    }
};

inline void task_event(uint64_t event, uint64_t arg, uint64_t id = current_task_id) {
    g_tracer.add(event, id, arg);
}

class task {
public:
    task_id _id;
protected:
    scheduling_group _sg;
private:
#ifdef SEASTAR_TASK_BACKTRACE
    shared_backtrace _bt;
#endif
protected:
    // Task destruction is performed by run_and_dispose() via a concrete type,
    // so no need for a virtual destructor here. Derived classes that implement
    // run_and_dispose() should be declared final to avoid losing concrete type
    // information via inheritance.
    ~task() = default;

    scheduling_group set_scheduling_group(scheduling_group new_sg) noexcept{
        return std::exchange(_sg, new_sg);
    }
public:
    explicit task(scheduling_group sg = current_scheduling_group()) noexcept : _sg(sg) {}
    virtual void run_and_dispose() noexcept = 0;
    /// Returns the next task which is waiting for this task to complete execution, or nullptr.
    virtual task* waiting_task() noexcept = 0;
    scheduling_group group() const { return _sg; }
    shared_backtrace get_backtrace() const;
#ifdef SEASTAR_TASK_BACKTRACE
    void make_backtrace() noexcept;
#else
    void make_backtrace() noexcept {}
#endif
};

inline
shared_backtrace task::get_backtrace() const {
#ifdef SEASTAR_TASK_BACKTRACE
    return _bt;
#else
    return {};
#endif
}

SEASTAR_MODULE_EXPORT_BEGIN

void schedule(task* t) noexcept;
void schedule_checked(task* t) noexcept;
void schedule_urgent(task* t) noexcept;

SEASTAR_MODULE_EXPORT_END
}
