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

#include <utility>

namespace seastar {

class task_slist;

class task {
private:
    friend class task_slist;
    union {
        unsigned _scheduling_group_id;
        task* _next;
    };

    static uintptr_t disguise_sched_group(scheduling_group sg) noexcept {
        unsigned id = internal::scheduling_group_index(sg);
        return (id << 1) | 0x1;
    }
    static scheduling_group unveil_sched_group(uintptr_t val) noexcept {
        SEASTAR_ASSERT(val & 0x1);
        return internal::scheduling_group_from_index(val >> 1);
    }
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
        return unveil_sched_group(std::exchange(_scheduling_group_id, disguise_sched_group(new_sg)));
    }
public:
    explicit task(scheduling_group sg = current_scheduling_group()) noexcept : _scheduling_group_id(disguise_sched_group(sg)) {}
    virtual void run_and_dispose() noexcept = 0;
    /// Returns the next task which is waiting for this task to complete execution, or nullptr.
    virtual task* waiting_task() noexcept = 0;
    scheduling_group group() const { return unveil_sched_group(_scheduling_group_id); }
#ifdef SEASTAR_TASK_BACKTRACE
    void make_backtrace() noexcept;
    shared_backtrace get_backtrace() const { return _bt; }
#else
    void make_backtrace() noexcept {}
    shared_backtrace get_backtrace() const { return {}; }
#endif
};

// The sched_group disguising/unveiling (see above) assumes that
// the task* always has its zero bit cleared
static_assert(alignof(task) > 1, "task pointer must not occupy zero bit");

class task_slist {
    task* _first;
    task** _last_p;
    size_t _size;

public:
    task_slist() noexcept : _first(0), _last_p(&_first), _size(0) {}
    task_slist(const task_slist&) = delete;
    task_slist(task_slist&&) = delete;

    void push_back(task* t) noexcept {
        SEASTAR_ASSERT(t->_scheduling_group_id & 0x1);
        t->_next = nullptr;
        *_last_p = t;
        _last_p = &t->_next;
        _size++;
    }

    void push_front(task* t) noexcept {
        SEASTAR_ASSERT(t->_scheduling_group_id & 0x1);
        t->_next = _first;
        _first = t;
        if (_last_p == &_first) {
            _last_p = &t->_next;
        }
        _size++;
    }

    task* pop_front(scheduling_group current) noexcept {
        task* f = _first;
        _first = f->_next;
        if (_last_p == &f->_next) {
            _last_p = &_first;
        }
        f->_scheduling_group_id = task::disguise_sched_group(current);
        _size--;
        return f;
    }

    bool empty() const noexcept {
        return _first == 0;
    }

    size_t size() const noexcept {
        return _size;
    }

    template <typename Fn>
    void do_for_each(Fn&& fn) const {
        for (const task* t = _first; t != nullptr; t = t->_next) {
            fn(t);
        }
    }
};

void schedule(task* t) noexcept;
void schedule_checked(task* t) noexcept;
void schedule_urgent(task* t) noexcept;

}
